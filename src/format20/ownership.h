// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_OWNERSHIP_H
#define INFS20_OWNERSHIP_H
#include "store.h"

/*
 * Executable S2 lifetime model. Records are stored in the same committed
 * metadata tree as the S1 store, not a sidecar journal or backup namespace.
 * The arrays bound model exploration only; production requires paged indexes,
 * arena accounting and real allocation/data I/O. A node is an immutable
 * semantic identity, with a separately versioned physical owner range.
 */
#define INFS20_MODEL_NODES 32u
#define INFS20_MODEL_EDGES 96u
enum infs20_model_kind {
    INFS20_MODEL_ROOT = 1, INFS20_MODEL_PAGE = 2, INFS20_MODEL_DATA = 3
};
enum infs20_model_state {
    INFS20_MODEL_LIVE = 1, INFS20_MODEL_PENDING = 2,
    INFS20_MODEL_DRAINING = 3, INFS20_MODEL_RELEASABLE = 4
};
enum infs20_model_role {
    INFS20_MODEL_STRONG = 0, INFS20_MODEL_ACTIVE = 1,
    INFS20_MODEL_RETIRED = 2, INFS20_MODEL_SNAPSHOT = 3,
    INFS20_MODEL_FALLBACK = 4, INFS20_MODEL_OPERATION = 5
};
struct infs20_model_node {
    uint8_t id[16];
    uint64_t start, blocks, refs;
    uint32_t rank;
    uint8_t kind, state;
};
struct infs20_model_edge {
    uint8_t source[16], target[16];
    uint64_t ordinal;
    uint8_t role;
};
struct infs20_model_graph {
    struct infs20_model_node nodes[INFS20_MODEL_NODES];
    struct infs20_model_edge edges[INFS20_MODEL_EDGES];
    size_t node_count, edge_count;
};

/* Independent graph trace checks counts, acyclicity, unique edge keys,
 * active authority, valid roles and exclusive physical ownership. */
infs_status infs20_model_validate(const struct infs20_store_layout *,
                                  const struct infs20_model_graph *);

/* All mutations are private-candidate transitions. Persist the complete
 * validated result with model_commit; an error leaves the caller's graph
 * unchanged. Native retention is represented as strong root pins. */
infs_status infs20_model_retire_activate(const struct infs20_store_layout *,
    struct infs20_model_graph *, const uint8_t new_root[16], uint64_t retired_pin);
infs_status infs20_model_reactivate(const struct infs20_store_layout *,
    struct infs20_model_graph *, uint64_t retired_pin);
infs_status infs20_model_drop_pin(const struct infs20_store_layout *,
    struct infs20_model_graph *, uint64_t ordinal);
infs_status infs20_model_reclaim_step(const struct infs20_store_layout *,
    struct infs20_model_graph *, const uint8_t id[16]);
/* A releasable node can leave this logical ledger only after external
 * fallback/reader retirement has been established by the caller. This model
 * does not issue discard or reuse a physical range. */
infs_status infs20_model_release(const struct infs20_store_layout *,
    struct infs20_model_graph *, const uint8_t id[16], int retirement_proven);

infs_status infs20_model_commit(const struct infs20_store_layout *,
    const struct infs20_store_io *, const uint8_t tx[16],
    const struct infs20_model_graph *, struct infs20_store_state *);
/* Integrate additional validated semantic records in the same commit.
 * The extension caller must validate their relationships with the graph. */
/* One canonical ownership encoder, shared by both experimental storage
 * profiles. The callback borrows the sorted record array only for the call. */
typedef infs_status (*infs20_model_records_fn)(void *,
    const struct infs20_store_record *,size_t);
infs_status infs20_model_with_records(const struct infs20_store_layout *,
    const struct infs20_model_graph *,const struct infs20_store_record *,size_t,
    infs20_model_records_fn,void *);
infs_status infs20_model_commit_extended(const struct infs20_store_layout *,
    const struct infs20_store_io *, const uint8_t tx[16],
    const struct infs20_model_graph *, const struct infs20_store_record *,
    size_t, infs20_store_prepare, void *, struct infs20_store_state *);
infs_status infs20_model_decode_record(void *,const struct infs20_record_view *);
infs_status infs20_model_load(const struct infs20_store_layout *,
    const struct infs20_store_io *, struct infs20_model_graph *);
#endif
