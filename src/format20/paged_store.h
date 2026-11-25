// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_PAGED_STORE_H
#define INFS20_PAGED_STORE_H
#include "store.h"
#include "tree.h"

/*
 * Experimental integration of the immutable tree with the existing commit
 * authority. The fixed bootstrap banks carry only the committed descriptor
 * (root and arena frontiers), never a parallel copy of the namespace.
 * Three disjoint arena ranges support append and explicit verified reuse;
 * production still needs dynamic arenas and scalable ownership indexes.
 * Caller holds exclusive, immutable access throughout each operation.
 */
struct infs20_paged_layout {
    struct infs20_store_layout bootstrap;
    uint64_t arena_start[INFS20_LANE_COUNT];
    uint64_t arena_blocks[INFS20_LANE_COUNT];
};
#define INFS20_PAGED_BATCH_MAX 256u
#define INFS20_ARENA_FREE_MAX 128u
struct infs20_arena_run { uint64_t start,blocks; };
struct infs20_paged_change {
    const uint8_t *key,*value;
    size_t key_size,value_size;
    int remove;
};
struct infs20_paged_state {
    struct infs20_store_state commit;
    struct infs20_tree_ref root;
    uint64_t used[INFS20_LANE_COUNT];
    size_t free_count;
    struct infs20_arena_run free_runs[INFS20_ARENA_FREE_MAX];
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
/* Apply ordered changes to one private root; publish once. The optional
 * prepare callback stages caller-reserved payload before metadata writes,
 * under the same dependency barrier. A late invalid/missing key or arena
 * exhaustion makes no storage writes and never calls prepare. Explicit
 * create_empty permits a first transaction only on blank bootstrap banks.
 * Intermediate private path pages may occupy committed arena space until
 * explicit arena collection retires their fallback dependencies. */
infs_status infs20_paged_apply(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],
    const struct infs20_paged_change *,size_t,int create_empty,
    infs20_store_prepare,infs20_store_prepare,void *,struct infs20_paged_state *);
/* Transitional bounded-model adapter: diff sorted canonical records
 * against the committed tree and apply only changed keys atomically.
 * This performs a full model traversal; production foreground operations
 * must use targeted batches, not whole-volume replacement. */
infs_status infs20_paged_replace(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],
    const struct infs20_store_record *,size_t,int create_empty,
    infs20_store_prepare,infs20_store_prepare,void *,struct infs20_paged_state *);
infs_status infs20_paged_get(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t *,size_t,uint8_t *,size_t,size_t *);
infs_status infs20_paged_visit(const struct infs20_paged_layout *,
    const struct infs20_store_io *,infs20_tree_visitor,void *);

/* Bounded key-range batches against one selected commit. Pass zero
 * expected_sequence on the first call and its returned sequence on later
 * calls. A different authority returns BUSY before any callback. This
 * does not substitute for full offline proof or protection validation. */
infs_status infs20_paged_visit_range(const struct infs20_paged_layout *,
    const struct infs20_store_io *,uint64_t expected_sequence,
    const uint8_t *start,size_t start_size,int exclusive,
    const uint8_t *end,size_t end_size,size_t limit,infs20_tree_visitor,
    void *,size_t *seen,int *more,uint64_t *sequence);

/*
 * Run a read operation against one immutable committed view. The tree I/O
 * and root are borrowed for the callback only; the caller must maintain
 * exclusive immutable access. This is the common indexed-read boundary,
 * not an independently selectable root supplied by an untrusted caller.
 */
typedef infs_status (*infs20_paged_view_fn)(void *,
    const struct infs20_tree_io *,const struct infs20_tree_ref *,uint64_t);
infs_status infs20_paged_read_view(const struct infs20_paged_layout *,
    const struct infs20_store_io *,infs20_paged_view_fn,void *);

/* Explicit offline discovery of the original experimental region. Only
 * read callbacks are exposed. The immutable raw source must remain valid
 * while using the returned region. Full tree proof is checked before
 * reporting a recoverable region. */
infs_status infs20_paged_discover(const struct infs20_raw_source *,
    struct infs20_store_region *,struct infs20_paged_layout *,
    struct infs20_paged_state *);
/* Explicit metadata maintenance. Trace the current committed tree, then
 * publish the SAME root in a successor bank with proven unreachable runs.
 * Only after full publication may the next mutation consume those runs:
 * both selectable banks then reference the same pre-collection tree.
 * Memory is proportional to visited live metadata, not device capacity.
 * This bounded candidate refuses an unrepresentable free-run inventory. */
infs_status infs20_paged_collect(const struct infs20_paged_layout *,
    const struct infs20_store_io *,const uint8_t tx[16],struct infs20_paged_state *);
#endif
