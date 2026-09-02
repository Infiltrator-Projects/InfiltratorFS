// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/fs.h"
#include "infilfs/iac1.h"
#include "infilfs/storage.h"
#include "infilfs/utf8.h"
#include "infiltratr/core.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <lz4.h>

#define INFS_DIRENT_ALIGN 8u

static const uint8_t snapshot_catalog_id[16] = INFS_SNAPSHOT_CATALOG_ID;

static infs_status transaction_next_generation(const struct infs_volume *vol,
                                               uint64_t *generation)
{
    if (!vol || !generation)
        return INFS_STATUS_INVALID_ARGUMENT;
    uint64_t current = infs_le64_to_cpu(vol->tx_base_sb.generation);
    if (!infiltratr_u64_add_checked(current, UINT64_C(1), generation) ||
        *generation == 0)
        return INFS_STATUS_OVERFLOW;
    return INFS_STATUS_OK;
}

static int validate_common_metadata(
    const struct infs_attributes_disk *attributes,
    const struct infs_posix_compat_disk *posix);
static int validate_integrity_metadata(struct infs_volume *vol);
static int validate_namespace_graph(struct infs_volume *vol);
static int validate_checksum_graph(struct infs_volume *vol);
static infs_status generate_unique_object_id(struct infs_volume *vol,
                                             uint8_t id[16]);
static infs_status file_free_unshared_run(
    struct infs_volume *vol, const uint8_t owner_id[16],
    uint64_t start, uint64_t count);
static infs_status file_read_small_content(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    uint8_t data[INFS_INLINE_DATA_MAX]);
static infs_status file_store_inline(
    struct infs_volume *vol, uint8_t object[INFS_BLOCK_SIZE],
    const uint8_t *data, size_t size);
static infs_status snapshot_union_bitmap(struct infs_volume *vol,
                                         uint8_t **union_out);
static int build_live_ownership_bitmap(
    struct infs_volume *vol, const struct infs_index_entry_disk *entries,
    uint32_t count, uint8_t *owners);
static void tx_abort(struct infs_volume *vol);
static void directory_cache_clear(struct infs_volume *vol);
static void directory_cache_destroy(struct infs_volume *vol);
static void directory_cache_init(struct infs_volume *vol);
static int object_cache_rebuild(struct infs_volume *vol);

static int file_validate_volume(
    struct infs_volume *vol, uint8_t block[INFS_BLOCK_SIZE],
    struct infs_file_payload_disk **payload_out,
    struct infs_extent_disk **extents_out);
static int data_block_verify(
    struct infs_volume *vol, struct infs_file_payload_disk *file,
    const uint8_t owner_id[16], uint64_t logical,
    const uint8_t data[INFS_BLOCK_SIZE]);

static uint32_t extent_kind(uint32_t flags)
{
    return flags & INFS_EXTENT_KIND_MASK;
}

static uint32_t extent_codec(uint32_t flags)
{
    uint32_t low =
        (flags & INFS_EXTENT_CODEC_MASK) >> INFS_EXTENT_CODEC_SHIFT;
    uint32_t high =
        (flags & INFS_EXTENT_CODEC_EXT_MASK) >> INFS_EXTENT_CODEC_EXT_SHIFT;
    return low | (high << 2u);
}

static uint32_t extent_stored_bytes(uint32_t flags)
{
    return (flags & INFS_EXTENT_STORED_BYTES_MASK) >>
        INFS_EXTENT_STORED_BYTES_SHIFT;
}

static int extent_is_compressed(uint32_t flags)
{
    return extent_kind(flags) == INFS_EXTENT_NORMAL &&
        extent_codec(flags) != INFS_COMPRESSION_NONE;
}

static uint64_t extent_physical_blocks(uint32_t logical_blocks, uint32_t flags)
{
    uint32_t stored;

    if (extent_kind(flags) == INFS_EXTENT_HOLE)
        return 0;
    if (!extent_is_compressed(flags))
        return logical_blocks;
    stored = extent_stored_bytes(flags);
    return stored / INFS_BLOCK_SIZE + ((stored % INFS_BLOCK_SIZE) != 0);
}

static int extent_flags_valid(uint32_t logical_blocks, uint64_t physical,
                              uint32_t flags)
{
    uint32_t kind = extent_kind(flags);
    uint32_t codec = extent_codec(flags);
    uint32_t stored = extent_stored_bytes(flags);

    if (!logical_blocks)
        return 0;
    if (kind == INFS_EXTENT_HOLE)
        return flags == INFS_EXTENT_HOLE && physical == 0;
    if (kind != INFS_EXTENT_NORMAL || physical == 0)
        return 0;
    if (codec == INFS_COMPRESSION_NONE)
        return flags == INFS_EXTENT_NORMAL;
    if ((codec != INFS_COMPRESSION_LZ4 &&
         codec != INFS_COMPRESSION_IAC1) || !stored ||
        logical_blocks > INFS_COMPRESSION_CLUSTER_BLOCKS ||
        stored > INFS_EXTENT_STORED_BYTES_MAX ||
        (uint64_t)stored >= (uint64_t)logical_blocks * INFS_BLOCK_SIZE)
        return 0;
    return 1;
}

static uint32_t extent_compressed_flags(uint32_t codec, uint32_t stored_bytes)
{
    uint32_t low = (codec & 0x3u) << INFS_EXTENT_CODEC_SHIFT;
    uint32_t high = ((codec >> 2u) << INFS_EXTENT_CODEC_EXT_SHIFT) &
        INFS_EXTENT_CODEC_EXT_MASK;
    uint32_t stored = (stored_bytes << INFS_EXTENT_STORED_BYTES_SHIFT) &
        INFS_EXTENT_STORED_BYTES_MASK;
    return INFS_EXTENT_NORMAL | low | high | stored;
}

static int file_replace_range(struct infs_volume *vol,
                              uint8_t object[INFS_BLOCK_SIZE],
                              uint64_t logical_start, uint64_t block_count,
                              uint64_t new_physical, uint32_t new_flags);

static int namespace_object_type(uint16_t type)
{
    return type == INFS_OBJECT_DIRECTORY || type == INFS_OBJECT_FILE ||
        type == INFS_OBJECT_SYMLINK;
}

static int symbolic_links_enabled(const struct infs_volume *vol)
{
    return vol &&
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SYMBOLIC_LINKS) != 0;
}

static int hard_links_enabled(const struct infs_volume *vol)
{
    return vol &&
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_HARD_LINKS) != 0;
}

static int snapshots_enabled(const struct infs_volume *vol)
{
    return vol &&
        (infs_le64_to_cpu(vol->sb.incompat_flags) &
         INFS_INCOMPAT_SNAPSHOTS) != 0;
}

/* Format 0.17 paged-index dispatch targets. part1 owns the classic index
 * implementation and calls these when it encounters a version-2 index head. */
static int paged_index_find(struct infs_volume *vol, const uint8_t id[16],
                            struct infs_lookup *out);
static int paged_index_repoint(struct infs_volume *vol, const uint8_t id[16],
                               uint64_t object_block, uint16_t type);
static int paged_index_add(struct infs_volume *vol, const uint8_t id[16],
                           uint64_t object_block, uint16_t type);
static int paged_index_remove(struct infs_volume *vol, const uint8_t id[16]);

/* Format 0.17 scalable object-index radix tree. */
static int tree_index_find(struct infs_volume *vol, const uint8_t id[16],
                           struct infs_lookup *out);
static int tree_index_repoint(struct infs_volume *vol, const uint8_t id[16],
                              uint64_t object_block, uint16_t type);
static int tree_index_add(struct infs_volume *vol, const uint8_t id[16],
                          uint64_t object_block, uint16_t type);
static int tree_index_remove(struct infs_volume *vol, const uint8_t id[16]);
static int tree_index_snapshot(struct infs_volume *vol,
                               struct infs_index_entry_disk **entries_out,
                               uint32_t *count_out);

/* These narrowly-scoped helpers are retained for planned cache/extent fast
 * paths but are deliberately dormant in Format 0.17.  Apply the unused
 * attribute to their declarations (rather than rewriting their identifiers
 * with macros), so GCC/Clang attach it reliably to the actual functions while
 * newly orphaned helpers still produce diagnostics. */
#if defined(__GNUC__) || defined(__clang__)
static int object_cache_lookup_page(struct infs_volume *vol,
                                    const uint8_t id[16],
                                    uint32_t *page_out)
    __attribute__((unused));
static uint64_t bitmap_free_count(const struct infs_volume *vol)
    __attribute__((unused));
static uint64_t *metadata_head_page_pointers(void *payload)
    __attribute__((unused));
static int paged_extent_replace(struct infs_volume *vol,
                                uint8_t object[INFS_BLOCK_SIZE],
                                struct infs_file_payload_disk *file,
                                uint64_t logical_start, uint64_t block_count,
                                uint64_t new_physical, uint32_t new_flags)
    __attribute__((unused));
#endif

#include "volume/part1.inc"
#include "volume/allocation-map.inc"
#include "volume/phase3/runtime-cache.inc"
#include "volume/phase3/paged-metadata.inc"
#include "volume/phase3/index-tree.inc"
#include "volume/phase3/directory-tree.inc"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
/* total_count is uint32_t.  The overflow guard in paged-extents is required
 * for 32-bit size_t builds and is provably false on 64-bit GCC; retain the
 * portable guard without accepting unrelated -Wtype-limits diagnostics. */
#pragma GCC diagnostic ignored "-Wtype-limits"
#endif
#include "volume/phase3/paged-extents.inc"
#include "volume/phase3/compression.inc"
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
#include "volume/phase3/part2-01.inc"
#include "volume/phase3/part2-02.inc"
#include "volume/phase3/part2-03.inc"
#include "volume/phase3/part2-04.inc"
#include "volume/phase3/part3-01.inc"
#include "volume/phase3/part3-02.inc"
#include "volume/phase3/part3-03.inc"
#include "volume/phase3/part3-04.inc"
#include "volume/phase3/part3-05.inc"
#include "volume/phase3/part4-snapshot.inc"
#include "volume/phase3/resize.inc"
#include "volume/integrity.inc"
#include "volume/phase3/part4-reflink.inc"
#include "volume/phase3/part4-inline.inc"
#include "volume/phase3/part4-01.inc"
#include "volume/phase3/part4-02.inc"
#include "volume/phase3/part4-03.inc"
#include "volume/phase3/part4-04.inc"
#include "volume/phase3/part4-05.inc"
#include "volume/phase3/part5-01.inc"
#include "volume/phase3/part5-02.inc"
#include "volume/phase3/part5-03.inc"

/*
 * Private regression hooks for the allocator index. They are intentionally
 * absent from the installed public headers; tests link against the production
 * core and exercise the exact implementation used by every userspace adapter.
 */
int infs_internal_test_free_extent_rebuild(struct infs_volume *vol)
{
    return free_extent_index_rebuild(vol);
}

void infs_internal_test_free_extent_destroy(struct infs_volume *vol)
{
    free_extent_index_destroy(vol);
}

int infs_internal_test_free_extent_remove(struct infs_volume *vol,
                                          uint64_t start, uint64_t count)
{
    return free_extent_index_remove(vol, start, count);
}

int infs_internal_test_free_extent_add(struct infs_volume *vol,
                                       uint64_t start, uint64_t count)
{
    return free_extent_index_add(vol, start, count);
}

int infs_internal_test_free_extent_choose_forward(
    const struct infs_volume *vol, uint64_t wanted, uint64_t cursor,
    uint64_t *start_out, uint64_t *count_out)
{
    return free_extent_index_choose_forward(
        vol, wanted, cursor, start_out, count_out);
}

int infs_internal_test_free_extent_choose_reverse(
    const struct infs_volume *vol, uint64_t wanted, uint64_t cursor,
    int exact, uint64_t *start_out, uint64_t *count_out)
{
    return free_extent_index_choose_reverse(
        vol, wanted, cursor, exact, start_out, count_out);
}

