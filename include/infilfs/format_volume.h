// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_FORMAT_VOLUME_H
#define INFILFS_FORMAT_VOLUME_H

#include "infilfs/storage.h"

/* Construct a fresh InfiltratorFS filesystem on an already-open, exclusively
 * held storage target. The caller owns target selection/locking. On any
 * failure after destructive writes begin, checkpoints remain invalid unless
 * the final publication completed successfully. */
infs_status infs_format_storage(struct infs_storage *storage, const char *label);

#endif
