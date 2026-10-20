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
 * Explicit per-object protection counts exceeding the configured number of
 * members are rejected before I/O, never silently weakened.
 *
 * The destination must be a distinct, otherwise unused storage handle, not
 * an element of the supplied member array. Every member must independently
 * own a distinct backing context; duplicate contexts would be closed twice
 * and cannot provide independent redundancy. Aliasing is rejected without
 * touching the caller-owned members.
 */
infs_status infs_storage_mirror_create(struct infs_storage *members,
                                       size_t member_count,
                                       struct infs_storage *out);

#endif
