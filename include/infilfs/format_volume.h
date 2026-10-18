// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FORMAT_VOLUME_H
#define INFILFS_FORMAT_VOLUME_H

#include "infilfs/storage.h"

/* Construct a fresh InfiltratorFS filesystem on an already-open, exclusively
 * held storage target. The caller owns target selection/locking. On any
 * failure after destructive writes begin, checkpoints remain invalid unless
 * the final publication completed successfully. */
#define INFS_FORMAT_OPTION_REMOVABLE_NAMES_V1 UINT32_C(0x00000001)
#define INFS_FORMAT_OPTION_CASEFOLD_V1        UINT32_C(0x00000002)
#define INFS_KNOWN_FORMAT_OPTIONS \
    (INFS_FORMAT_OPTION_REMOVABLE_NAMES_V1 | INFS_FORMAT_OPTION_CASEFOLD_V1)

infs_status infs_format_storage(struct infs_storage *storage, const char *label);
infs_status infs_format_storage_with_options(struct infs_storage *storage,
                                             const char *label,
                                             uint32_t options);

/* Experimental Format 0.19 fixture creation for in-memory/regular-file
 * qualification only. This rejects physical block-device storage targets.
 * It does NOT imply the incomplete format is safe for mount or production. */
infs_status infs_format_storage_019_test_image(
    struct infs_storage *storage, const char *label);

#endif
