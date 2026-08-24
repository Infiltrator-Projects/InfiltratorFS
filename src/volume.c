// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/utf8.h"
#include "infiltratr/core.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define INFS_DIRENT_ALIGN 8u

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

/* Format 0.8 paged-index dispatch targets. part1 owns the classic index
 * implementation and calls these when it encounters a version-2 index head. */
static int paged_index_find(struct infs_volume *vol, const uint8_t id[16],
                            struct infs_lookup *out);
static int paged_index_repoint(struct infs_volume *vol, const uint8_t id[16],
                               uint64_t object_block, uint16_t type);
static int paged_index_add(struct infs_volume *vol, const uint8_t id[16],
                           uint64_t object_block, uint16_t type);
static int paged_index_remove(struct infs_volume *vol, const uint8_t id[16]);

#include "volume/part1.inc"
#include "volume/phase3/paged-metadata.inc"
#include "volume/phase3/part2-01.inc"
#include "volume/phase3/part2-02.inc"
#include "volume/phase3/part2-03.inc"
#include "volume/phase3/part2-04.inc"
#include "volume/phase3/part3-01.inc"
#include "volume/phase3/part3-02.inc"
#include "volume/phase3/part3-03.inc"
#include "volume/phase3/part3-04.inc"
#include "volume/phase3/part3-05.inc"
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
