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

static int checksum_set_zero_range(struct infs_volume *vol,
                                   struct infs_file_payload_disk *file,
                                   const uint8_t owner_id[16],
                                   uint64_t logical, uint64_t count);
static int validate_integrity_metadata(struct infs_volume *vol);

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
#include "volume/phase3/part4-01.inc"
#include "volume/phase3/part4-02.inc"
#include "volume/phase3/part4-03.inc"
#include "volume/phase3/part4-04.inc"
#include "volume/phase3/part5-01.inc"
#include "volume/phase3/part5-02.inc"
#include "volume/phase3/part5-03.inc"
