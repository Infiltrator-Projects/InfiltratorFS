// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_STORAGE_MIRROR_H
#define INFILFS_STORAGE_MIRROR_H

#include <stddef.h>
#include "infilfs/storage.h"

/*
 * Construct a synchronous replicated storage backend.
 *
 * On success ownership of every member context is transferred into the mirror
 * and the supplied member handles are cleared. The visible capacity is the
 * smallest member capacity. Reads try members in order until one succeeds;
 * writes and durability flushes are issued to every member and report failure
 * if any replica fails. This keeps replication below filesystem semantics:
 * every member contains a complete ordinary InfiltratorFS image.
 */
infs_status infs_storage_mirror_create(struct infs_storage *members,
                                       size_t member_count,
                                       struct infs_storage *out);

#endif
