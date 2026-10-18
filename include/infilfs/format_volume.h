// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FORMAT_VOLUME_H
#define INFILFS_FORMAT_VOLUME_H

#include "infilfs/storage.h"

/* Construct a fresh InfiltratorFS filesystem on an already-open, exclusively
 * held storage target. The caller owns target selection/locking. On any
 * failure after destructive writes begin, the new volume may be incomplete
 * or degraded; only successful completion establishes a complete format. */
#define INFS_FORMAT_OPTION_REMOVABLE_NAMES_V1 UINT32_C(0x00000001)
#define INFS_FORMAT_OPTION_CASEFOLD_V1        UINT32_C(0x00000002)
#define INFS_KNOWN_FORMAT_OPTIONS \
    (INFS_FORMAT_OPTION_REMOVABLE_NAMES_V1 | INFS_FORMAT_OPTION_CASEFOLD_V1)

infs_status infs_format_storage(struct infs_storage *storage, const char *label);
infs_status infs_format_storage_with_options(struct infs_storage *storage,
                                             const char *label,
                                             uint32_t options);
infs_status infs_format_storage_with_root_owner(struct infs_storage *storage,
    const char *label, uint32_t options, uint32_t root_uid, uint32_t root_gid);

/* Isolated fixture creation for in-memory/regular-file qualification.
 * Unlike the normal formatter, this rejects physical block-device targets. */
infs_status infs_format_storage_019_test_image(
    struct infs_storage *storage, const char *label);

#endif
