// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/volume.h"

#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/utf8.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#define INFS_DIRENT_ALIGN 8u

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

#include "volume/part1.inc"
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
