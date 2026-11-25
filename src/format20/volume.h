// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_VOLUME_MODEL_H
#define INFS20_VOLUME_MODEL_H
#include "ownership.h"
#include "paged_store.h"

/*
 * Executable single-device integration model. Disposable test media only:
 * flat ASCII names, bounded contiguous multi-block allocations, bounded metadata,
 * bounded payload reuse with fallback quarantine, no encryption or native
 * mount. Metadata arenas use explicit verified collection.
 * All calls require exclusive access. On an I/O failure the caller must
 * discard volatile state and recover cold before attempting another write.
 * IDs derive from a fresh nonzero transaction ID for every attempt.
 */
#define INFS20_VOLUME_NAME_MAX 63u
#define INFS20_VOLUME_FILES 64u
/* Model limit only: the production design uses paged allocation/checksum
 * trees, not this fixed upper bound or a contiguous-run requirement. */
#define INFS20_VOLUME_UNITS 16u
#define INFS20_VOLUME_BYTES (INFS20_VOLUME_UNITS * INFS20_PAGE_BYTES)

struct infs20_recovery_mapping {
    uint8_t root_id[16], allocation_id[16];
    uint64_t pin;
    uint8_t role;
    const char *name;
    size_t size;
};
struct infs20_recovery_unit {
    uint8_t allocation_id[16];
    uint64_t physical_block;
    size_t logical_offset, logical_size;
    infs_status status;
    /* Borrowed verified bytes, NULL on any unavailable/corrupt unit. */
    const uint8_t *data;
};
typedef infs_status (*infs20_recovery_mapping_fn)(void *,
    const struct infs20_recovery_mapping *);
typedef infs_status (*infs20_recovery_unit_fn)(void *,
    const struct infs20_recovery_unit *);

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
/* Verify only the units intersecting the requested logical range. A request
 * beyond EOF returns a successful short read. The output and byte count
 * remain unchanged on failure; this is not an unverified salvage read. */
infs_status infs20_volume_read_range(const struct infs20_store_layout *,
    const struct infs20_store_io *,uint64_t pin,const char *,uint64_t offset,
    uint8_t *,size_t,size_t *);
/* Enumerate the fully validated committed namespace across active/retained
 * pins, then each referenced allocation's units exactly once. Payload
 * failures are reported per unit; structural errors abort before callbacks.
 * Callbacks borrow values only during invocation. The caller must keep the
 * source immutable/exclusive and direct exports elsewhere. No source writes. */
infs_status infs20_volume_recovery_visit(const struct infs20_store_layout *,
    const struct infs20_store_io *,infs20_recovery_mapping_fn,
    infs20_recovery_unit_fn,void *);
/* The paged profile uses exactly the same canonical namespace and ownership
 * implementation, but stores it in the variable-height authenticated tree.
 * It is the successor to the bounded-bank regression profile. */
infs_status infs20_paged_volume_create(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16]);
infs_status infs20_paged_volume_write(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *name,const uint8_t *data,size_t size);
infs_status infs20_paged_volume_clone(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *source,const char *target);
infs_status infs20_paged_volume_read(const struct infs20_paged_layout *,
    const struct infs20_store_io *,uint64_t ordinal,const char *name,uint8_t *out,size_t capacity,size_t *size);
infs_status infs20_paged_volume_read_range(const struct infs20_paged_layout *,
    const struct infs20_store_io *,uint64_t ordinal,const char *name,uint64_t offset,uint8_t *out,size_t capacity,size_t *size);
infs_status infs20_paged_volume_recovery_visit(const struct infs20_paged_layout *,
    const struct infs20_store_io *,infs20_recovery_mapping_fn mapping_fn,infs20_recovery_unit_fn unit_fn,void *context);
infs_status infs20_paged_volume_snapshot(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t ordinal);
infs_status infs20_paged_volume_format(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t ordinal);
infs_status infs20_paged_volume_unformat(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t ordinal);
infs_status infs20_paged_volume_relocate(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *name);
/* Explicit retention release and collection. No automatic history expiry.
 * A later fully protected commit must retire the preceding bank before
 * physical reuse. This bounded collector is not the production paged GC. */
infs_status infs20_paged_volume_collect(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16]);
infs_status infs20_paged_volume_unlink(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],const char *);
infs_status infs20_paged_volume_release_pin(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],uint64_t ordinal);
#endif
