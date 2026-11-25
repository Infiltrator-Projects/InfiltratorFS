// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_PAGED_STORE_H
#define INFS20_PAGED_STORE_H
#include "store.h"
#include "tree.h"

/*
 * Experimental integration of the immutable tree with the existing commit
 * authority. The fixed bootstrap banks carry only the committed descriptor
 * (root and arena frontiers), never a parallel copy of the namespace.
 * Three disjoint append-only arena ranges prove initial private allocation;
 * production needs dynamic arenas, reference reclamation and relocation.
 * Caller holds exclusive, immutable access throughout each operation.
 */
struct infs20_paged_layout {
    struct infs20_store_layout bootstrap;
    uint64_t arena_start[INFS20_LANE_COUNT];
    uint64_t arena_blocks[INFS20_LANE_COUNT];
};
struct infs20_paged_state {
    struct infs20_store_state commit;
    struct infs20_tree_ref root;
    uint64_t used[INFS20_LANE_COUNT];
};

/* No implicit creation. Create is allowed only on an explicitly empty
 * bootstrap region and uses the existing commit protocol. As with all
 * test profiles, no installed formatter or native device is implied. */
infs_status infs20_paged_create(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],struct infs20_paged_state *);
infs_status infs20_paged_open(const struct infs20_paged_layout *,
    const struct infs20_store_io *,struct infs20_paged_state *);

/* Mutation stages only changed tree pages. A single existing bootstrap
 * commit publishes the tree and arena frontier together after dependency
 * writes/flush. After any ambiguous I/O failure, discard volatile state
 * and reopen cold. Never reuse a transaction ID. */
infs_status infs20_paged_put(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],
    const uint8_t *,size_t,const uint8_t *,size_t,struct infs20_paged_state *);
infs_status infs20_paged_remove(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],
    const uint8_t *,size_t,struct infs20_paged_state *);
infs_status infs20_paged_get(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
infs_status infs20_paged_visit(const struct infs20_paged_layout *,
    const struct infs20_store_io *,infs20_tree_visitor,void *);

/* Explicit offline discovery of the original experimental region. Only
 * read callbacks are exposed. The immutable raw source must remain valid
 * while using the returned region. Full tree proof is checked before
 * reporting a recoverable region. */
infs_status infs20_paged_discover(const struct infs20_raw_source *,
    struct infs20_store_region *,struct infs20_paged_layout *,
    struct infs20_paged_state *);
#endif
