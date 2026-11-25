// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_VOLUME_MODEL_H
#define INFS20_VOLUME_MODEL_H
#include "ownership.h"

/*
 * Executable single-device integration model. Disposable test media only:
 * flat ASCII names, at most one physical block per file, bounded metadata,
 * append-only payload allocation (no reuse), no encryption or native mount.
 * All calls require exclusive access. On an I/O failure the caller must
 * discard volatile state and recover cold before attempting another write.
 * IDs derive from a fresh nonzero transaction ID for every attempt.
 */
#define INFS20_VOLUME_NAME_MAX 63u
#define INFS20_VOLUME_FILES 64u
infs_status infs20_volume_create(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16]);
infs_status infs20_volume_write(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *,
    const uint8_t *,size_t);
infs_status infs20_volume_clone(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *,const char *);
infs_status infs20_volume_read(const struct infs20_store_layout *,
    const struct infs20_store_io *,uint64_t pin,const char *,uint8_t *,size_t,size_t *);
infs_status infs20_volume_snapshot(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t pin);
infs_status infs20_volume_format(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t retired_pin);
infs_status infs20_volume_unformat(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t retired_pin);
infs_status infs20_volume_relocate(const struct infs20_store_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *);
#endif
