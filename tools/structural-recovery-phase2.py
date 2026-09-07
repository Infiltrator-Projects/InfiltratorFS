#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Replace fixed extent-page pointer arrays with a checksummed linked chain."""
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
            if c == "\n": in_line = False
        elif in_block:
            if c == "*" and n == "/": in_block = False; i += 1
        elif in_str:
            if esc: esc = False
            elif c == "\\": esc = True
            elif c == '"': in_str = False
        elif in_char:
            if esc: esc = False
            elif c == "\\": esc = True
            elif c == "'": in_char = False
        else:
            if c == "/" and n == "/": in_line = True; i += 1
            elif c == "/" and n == "*": in_block = True; i += 1
            elif c == '"': in_str = True
            elif c == "'": in_char = True
            elif c == "{": depth += 1
            elif c == "}":
                depth -= 1
                if depth == 0:
                    return text[:start] + replacement.rstrip() + "\n" + text[i + 1:]
        i += 1
    raise SystemExit(f"{label}: unterminated function")


path = "src/volume/paged-extents.inc"
text = read(path)
if "extent_page_next_block" not in text:
    marker = '''static uint64_t *file_extent_page_pointers(struct infs_file_payload_disk *file)\n{\n    return (uint64_t *)(file_extent_head(file) + 1);\n}\n'''
    helper = marker + r'''
/* The 4016-byte metadata payload has eight spare bytes after 167 24-byte
 * extent records. Use those bytes as a physical next-page link. The file head
 * therefore stores one root pointer rather than a bounded pointer vector. */
static uint64_t extent_page_next_block(const uint8_t block[INFS_BLOCK_SIZE])
{
    uint64_t encoded = 0;
    memcpy(&encoded, block + INFS_BLOCK_SIZE - sizeof(encoded), sizeof(encoded));
    return infs_le64_to_cpu(encoded);
}

static void extent_page_set_next_block(uint8_t block[INFS_BLOCK_SIZE],
                                       uint64_t next)
{
    uint64_t encoded = infs_cpu_to_le64(next);
    memcpy(block + INFS_BLOCK_SIZE - sizeof(encoded), &encoded, sizeof(encoded));
}
'''
    if text.count(marker) != 1:
        raise SystemExit("extent-chain helper anchor mismatch")
    text = text.replace(marker, helper, 1)

text = replace_function(text, "static int paged_extent_head_validate(", r'''static int paged_extent_head_validate(struct infs_volume *vol,
                                      uint8_t object[INFS_BLOCK_SIZE],
                                      struct infs_file_payload_disk *file)
{
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    struct infs_extent_head_disk *head;
    uint64_t *root_ptr;
    uint64_t root;
    uint64_t total;
    uint32_t pages;
    uint32_t total_extents;
    size_t need;

    if ((infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_PAGED_EXTENTS) == 0)
        return INFS_STATUS_CORRUPT;
    head = file_extent_head(file);
    pages = infs_le32_to_cpu(head->page_count);
    total_extents = infs_le32_to_cpu(file->extent_count);
    if (!pages || !total_extents || pages > total_extents ||
        infs_le32_to_cpu(head->reserved) != 0)
        return INFS_STATUS_CORRUPT;
    need = sizeof(*file) + sizeof(*head) + sizeof(uint64_t);
    if (need != infs_le32_to_cpu(hdr->payload_size) ||
        need > INFS_BLOCK_SIZE - sizeof(*hdr))
        return INFS_STATUS_CORRUPT;
    root_ptr = file_extent_page_pointers(file);
    root = infs_le64_to_cpu(root_ptr[0]);
    total = infs_le64_to_cpu(vol->sb.total_blocks);
    if (!root || root >= total || pages > total || !bitmap_get(vol->bitmap, root))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}''', "extent-chain head validation")

text = replace_function(text, "static int paged_extent_snapshot(", r'''static int paged_extent_snapshot(struct infs_volume *vol,
                                 uint8_t object[INFS_BLOCK_SIZE],
                                 struct infs_file_payload_disk *file,
                                 struct infs_extent_disk **entries_out,
                                 uint32_t *count_out)
{
    struct infs_extent_disk *all;
    struct infs_object_header_disk *hdr;
    struct infs_extent_head_disk *head;
    uint64_t *root_ptr;
    uint64_t page_block;
    uint64_t total_blocks;
    uint64_t next_logical = 0;
    uint32_t page_count;
    uint32_t total_count;
    uint32_t at = 0;
    infs_status status;

    if (!entries_out || !count_out)
        return INFS_STATUS_INVALID_ARGUMENT;
    *entries_out = NULL;
    *count_out = 0;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    total_count = infs_le32_to_cpu(file->extent_count);
    if ((size_t)total_count > SIZE_MAX / sizeof(*all))
        return INFS_STATUS_OVERFLOW;
    all = malloc((size_t)total_count * sizeof(*all));
    if (!all)
        return INFS_STATUS_NO_MEMORY;
    hdr = (struct infs_object_header_disk *)object;
    head = file_extent_head(file);
    page_count = infs_le32_to_cpu(head->page_count);
    root_ptr = file_extent_page_pointers(file);
    page_block = infs_le64_to_cpu(root_ptr[0]);
    total_blocks = infs_le64_to_cpu(vol->sb.total_blocks);

    for (uint32_t p = 0; p < page_count; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        struct infs_extent_disk *ext;
        uint32_t count;
        uint64_t next;

        if (!page_block || page_block >= total_blocks ||
            !bitmap_get(vol->bitmap, page_block)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        status = read_block(vol, page_block, page_data);
        if (status != INFS_STATUS_OK)
            goto fail;
        status = extent_page_validate(vol, page_data, hdr->object_id, &ext, &count);
        if (status != INFS_STATUS_OK)
            goto fail;
        if (count > total_count - at ||
            infs_le64_to_cpu(ext[0].logical_block) != next_logical) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        memcpy(&all[at], ext, (size_t)count * sizeof(*ext));
        at += count;
        next_logical = infs_le64_to_cpu(ext[count - 1u].logical_block) +
            infs_le32_to_cpu(ext[count - 1u].block_count);
        next = extent_page_next_block(page_data);
        if ((p + 1u < page_count &&
             (!next || next >= total_blocks || !bitmap_get(vol->bitmap, next))) ||
            (p + 1u == page_count && next != 0)) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
        page_block = next;
    }
    if (at != total_count) {
        status = INFS_STATUS_CORRUPT;
        goto fail;
    }
    {
        uint64_t logical_size = infs_le64_to_cpu(file->attributes.logical_size);
        uint64_t required = logical_size / INFS_BLOCK_SIZE +
            ((logical_size % INFS_BLOCK_SIZE) != 0);
        if (next_logical != required) {
            status = INFS_STATUS_CORRUPT;
            goto fail;
        }
    }
    *entries_out = all;
    *count_out = total_count;
    return INFS_STATUS_OK;
fail:
    free(all);
    return status;
}''', "extent-chain snapshot")

text = replace_function(text, "static int paged_extent_load_page(", r'''static int paged_extent_load_page(struct infs_volume *vol,
                                  uint8_t object[INFS_BLOCK_SIZE],
                                  struct infs_file_payload_disk *file,
                                  uint32_t page_index,
                                  uint8_t page_data[INFS_BLOCK_SIZE],
                                  struct infs_extent_disk **ext_out,
                                  uint32_t *count_out)
{
    struct infs_extent_head_disk *head = file_extent_head(file);
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint64_t *root_ptr = file_extent_page_pointers(file);
    uint64_t block;
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);
    infs_status status;

    if (page_index >= pages)
        return INFS_STATUS_CORRUPT;
    block = infs_le64_to_cpu(root_ptr[0]);
    for (uint32_t p = 0; p <= page_index; ++p) {
        if (!block || block >= total || !bitmap_get(vol->bitmap, block))
            return INFS_STATUS_CORRUPT;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id,
                                      ext_out, count_out);
        if (status != INFS_STATUS_OK)
            return status;
        if (p == page_index)
            return INFS_STATUS_OK;
        block = extent_page_next_block(page_data);
    }
    return INFS_STATUS_CORRUPT;
}''', "extent-chain load page")

text = replace_function(text, "static int paged_extent_find_page(", r'''static int paged_extent_find_page(struct infs_volume *vol,
                                  uint8_t object[INFS_BLOCK_SIZE],
                                  struct infs_file_payload_disk *file,
                                  uint64_t logical, uint32_t *page_out,
                                  uint8_t page_data[INFS_BLOCK_SIZE],
                                  struct infs_extent_disk **ext_out,
                                  uint32_t *count_out)
{
    struct infs_extent_head_disk *head = file_extent_head(file);
    struct infs_object_header_disk *hdr =
        (struct infs_object_header_disk *)object;
    uint32_t pages = infs_le32_to_cpu(head->page_count);
    uint64_t block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
    uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);

    for (uint32_t p = 0; p < pages; ++p) {
        struct infs_extent_disk *ext;
        uint32_t count;
        uint64_t first, end, next;
        infs_status status;

        if (!block || block >= total || !bitmap_get(vol->bitmap, block))
            return INFS_STATUS_CORRUPT;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id, &ext, &count);
        if (status != INFS_STATUS_OK)
            return status;
        first = infs_le64_to_cpu(ext[0].logical_block);
        end = infs_le64_to_cpu(ext[count - 1u].logical_block) +
            infs_le32_to_cpu(ext[count - 1u].block_count);
        if (logical >= first && logical < end) {
            if (page_out) *page_out = p;
            if (ext_out) *ext_out = ext;
            if (count_out) *count_out = count;
            return INFS_STATUS_OK;
        }
        if (logical < first)
            return INFS_STATUS_CORRUPT;
        next = extent_page_next_block(page_data);
        if ((p + 1u < pages && !next) || (p + 1u == pages && next))
            return INFS_STATUS_CORRUPT;
        block = next;
    }
    return INFS_STATUS_CORRUPT;
}''', "extent-chain find page")

text = replace_function(text, "static int paged_extent_rewrite_all(", r'''static int paged_extent_rewrite_all(struct infs_volume *vol,
                                    uint8_t object[INFS_BLOCK_SIZE],
                                    struct infs_file_payload_disk *file,
                                    const struct infs_extent_disk *entries,
                                    uint32_t count, int converting)
{
    struct infs_object_header_disk *hdr;
    uint64_t *old_ptr = NULL;
    uint32_t old_pages = 0;
    uint32_t new_pages;
    uint64_t next_block = 0;
    infs_status status = INFS_STATUS_OK;

    if (!count)
        return INFS_STATUS_INVALID_ARGUMENT;
    new_pages = (count + INFS_EXTENTS_PER_PAGE - 1u) / INFS_EXTENTS_PER_PAGE;
    if (!new_pages)
        return INFS_STATUS_OVERFLOW;
    hdr = (struct infs_object_header_disk *)object;

    if (!converting && file_has_paged_extents(object)) {
        struct infs_extent_head_disk *old_head = file_extent_head(file);
        uint64_t block;
        uint64_t total = infs_le64_to_cpu(vol->sb.total_blocks);

        status = paged_extent_head_validate(vol, object, file);
        if (status != INFS_STATUS_OK)
            return status;
        old_pages = infs_le32_to_cpu(old_head->page_count);
        if ((size_t)old_pages > SIZE_MAX / sizeof(*old_ptr))
            return INFS_STATUS_OVERFLOW;
        old_ptr = malloc((size_t)old_pages * sizeof(*old_ptr));
        if (!old_ptr)
            return INFS_STATUS_NO_MEMORY;
        block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
        for (uint32_t p = 0; p < old_pages; ++p) {
            uint8_t page_data[INFS_BLOCK_SIZE];
            if (!block || block >= total || !bitmap_get(vol->bitmap, block)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
            old_ptr[p] = block;
            status = read_block(vol, block, page_data);
            if (status != INFS_STATUS_OK)
                goto out;
            status = extent_page_validate(vol, page_data, hdr->object_id, NULL, NULL);
            if (status != INFS_STATUS_OK)
                goto out;
            block = extent_page_next_block(page_data);
            if ((p + 1u < old_pages && !block) ||
                (p + 1u == old_pages && block)) {
                status = INFS_STATUS_CORRUPT;
                goto out;
            }
        }
    }

    for (uint32_t p = new_pages; p-- > 0;) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint32_t at = p * INFS_EXTENTS_PER_PAGE;
        uint32_t chunk = count - at;
        uint64_t replacement = 0;
        uint64_t old_block = p < old_pages ? old_ptr[p] : 0;

        if (chunk > INFS_EXTENTS_PER_PAGE)
            chunk = INFS_EXTENTS_PER_PAGE;
        status = extent_page_encode(vol, hdr->object_id, &entries[at], chunk,
                                    page_data);
        if (status != INFS_STATUS_OK)
            goto out;
        extent_page_set_next_block(page_data, next_block);
        status = metadata_page_store(vol, old_block, page_data, &replacement);
        if (status != INFS_STATUS_OK)
            goto out;
        next_block = replacement;
    }

    for (uint32_t p = new_pages; p < old_pages; ++p) {
        free_run(vol, old_ptr[p], 1);
        if (vol->tx_error != INFS_STATUS_OK) {
            status = vol->tx_error;
            goto out;
        }
    }

    {
        struct infs_extent_head_disk *head = file_extent_head(file);
        uint64_t *root = file_extent_page_pointers(file);
        size_t payload = sizeof(*file) + sizeof(*head) + sizeof(uint64_t);
        memset(head, 0, INFS_BLOCK_SIZE - sizeof(*hdr) - sizeof(*file));
        head->page_count = infs_cpu_to_le32(new_pages);
        head->reserved = 0;
        root[0] = infs_cpu_to_le64(next_block);
        hdr->object_version = infs_cpu_to_le16(INFS_OBJECT_VERSION_PAGED);
        hdr->payload_size = infs_cpu_to_le32((uint32_t)payload);
        file->extent_count = infs_cpu_to_le32(count);
    }
    status = INFS_STATUS_OK;
out:
    free(old_ptr);
    return status;
}''', "extent-chain rewrite")

text = replace_function(text, "static int paged_extent_destroy_pages(", r'''static int paged_extent_destroy_pages(struct infs_volume *vol,
                                      uint8_t object[INFS_BLOCK_SIZE],
                                      struct infs_file_payload_disk *file)
{
    struct infs_object_header_disk *hdr;
    struct infs_extent_head_disk *head;
    uint64_t block;
    uint64_t total;
    uint32_t pages;
    infs_status status;

    if (!file_has_paged_extents(object))
        return INFS_STATUS_OK;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    hdr = (struct infs_object_header_disk *)object;
    head = file_extent_head(file);
    pages = infs_le32_to_cpu(head->page_count);
    block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
    total = infs_le64_to_cpu(vol->sb.total_blocks);
    for (uint32_t p = 0; p < pages; ++p) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t next;
        if (!block || block >= total || !bitmap_get(vol->bitmap, block))
            return INFS_STATUS_CORRUPT;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id, NULL, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        next = extent_page_next_block(page_data);
        if ((p + 1u < pages && !next) || (p + 1u == pages && next))
            return INFS_STATUS_CORRUPT;
        free_run(vol, block, 1);
        if (vol->tx_error != INFS_STATUS_OK)
            return vol->tx_error;
        block = next;
    }
    return INFS_STATUS_OK;
}''', "extent-chain destroy")

text = replace_function(text, "static int paged_extent_replace(struct infs_volume *vol,", r'''static int paged_extent_replace(struct infs_volume *vol,
                                uint8_t object[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk *file,
                                uint64_t logical_start, uint64_t block_count,
                                uint64_t new_physical, uint32_t new_flags)
{
    /* The linked representation has no head-array insertion ceiling. Rebuild
     * transaction-private pages as one CoW chain; page-local updates can be
     * reintroduced later without changing the disk graph. */
    return paged_extent_replace_rebuild(vol, object, file, logical_start,
                                        block_count, new_physical, new_flags);
}''', "extent-chain replace")

write(path, text)

own = read("src/volume/ownership-validation.inc")
own = replace_function(own, "static int claim_file_extent_metadata_pages(", r'''static int claim_file_extent_metadata_pages(struct infs_volume *vol,
                                            uint8_t *owners, uint64_t total,
                                            uint64_t object_block)
{
    uint8_t object[INFS_BLOCK_SIZE];
    struct infs_file_payload_disk *file;
    struct infs_object_header_disk *hdr;
    struct infs_extent_head_disk *head;
    uint64_t block;
    uint32_t pages;
    infs_status status = read_object(vol, object_block, object);

    if (status != INFS_STATUS_OK)
        return status;
    status = file_validate_volume(vol, object, &file, NULL);
    if (status != INFS_STATUS_OK)
        return status;
    if (!file_has_paged_extents(object))
        return INFS_STATUS_OK;
    status = paged_extent_head_validate(vol, object, file);
    if (status != INFS_STATUS_OK)
        return status;
    hdr = (struct infs_object_header_disk *)object;
    head = file_extent_head(file);
    pages = infs_le32_to_cpu(head->page_count);
    block = infs_le64_to_cpu(file_extent_page_pointers(file)[0]);
    for (uint32_t i = 0; i < pages; ++i) {
        uint8_t page_data[INFS_BLOCK_SIZE];
        uint64_t next;
        status = ownership_claim(owners, total, block);
        if (status != INFS_STATUS_OK)
            return status;
        status = read_block(vol, block, page_data);
        if (status != INFS_STATUS_OK)
            return status;
        status = extent_page_validate(vol, page_data, hdr->object_id, NULL, NULL);
        if (status != INFS_STATUS_OK)
            return status;
        next = extent_page_next_block(page_data);
        if ((i + 1u < pages && (!next || next >= total)) ||
            (i + 1u == pages && next))
            return INFS_STATUS_CORRUPT;
        block = next;
    }
    return INFS_STATUS_OK;
}''', "extent-chain ownership walk")
write("src/volume/ownership-validation.inc", own)

policy = ROOT / "tests/structural-overhaul-policy.sh"
p = policy.read_text(encoding="utf-8")
if "extent_page_next_block" not in p:
    p += """
# Extent heads must contain one root pointer; no fixed page-pointer ceiling may
# remain in portable extent validation or rewriting.
grep -Fq 'extent_page_next_block' "$root/src/volume/paged-extents.inc"
grep -Fq 'sizeof(*file) + sizeof(*head) + sizeof(uint64_t)' "$root/src/volume/paged-extents.inc"
! grep -Fq 'new_pages > INFS_EXTENT_PAGE_POINTERS' "$root/src/volume/paged-extents.inc"
! grep -Fq 'pages > INFS_EXTENT_PAGE_POINTERS' "$root/src/volume/paged-extents.inc"
"""
    policy.write_text(p, encoding="utf-8")

print("Structural recovery phase 2 portable extent chain applied.")
