// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_TREE_H
#define INFS20_TREE_H
#include "schema.h"

/*
 * Variable-height immutable authenticated tree candidate for Format 0.20.
 * Unlike the original fixed-bank executable model, a mutation copies only
 * the search path and split pages. Physical allocation and durable root
 * publication are deliberately caller-owned: this module must not invent
 * a second allocator or commit protocol. The candidate is not mounted.
 */
#define INFS20_TREE_KEY_MAX 1023u
#define INFS20_TREE_MAX_LEVEL 15u
#define INFS20_TREE_REF_BYTES 80u
#define INFS20_TREE_NODE_HEADER 8u

struct infs20_tree_ref {
    uint8_t id[16], digest[32];
    uint64_t sequence, blocks[INFS20_LANE_COUNT];
};
struct infs20_tree_io {
    void *context;
    uint8_t set_id[16], scope_id[16], member_id[16];
    uint64_t member_blocks, sequence;
    infs_status (*read)(void *,uint64_t,uint8_t *);
    infs_status (*write)(void *,uint64_t,const uint8_t *);
    /* Reserve three nonoverlapping private physical blocks, distinct from
     * ALL current/retained/fallback/transaction ownership. Return fresh
     * immutable ID, never a reused ID after an aborted publication.
     * The caller tracks reservations and releases them only after proof. */
    infs_status (*reserve)(void *,uint8_t[16],uint64_t[INFS20_LANE_COUNT]);
};
typedef infs_status (*infs20_tree_visitor)(void *,const uint8_t *,size_t,
                                          const uint8_t *,size_t);

/* An all-zero ref represents an empty tree. Outputs remain unchanged on
 * failure. New pages are private until the caller flushes and commits the
 * new root through the common transaction protocol. On failed mutation,
 * abandon its private reservations before retrying; do not publish a
 * partially written root. No code here overwrites a previously read page. */
infs_status infs20_tree_put(const struct infs20_tree_io *,
    const struct infs20_tree_ref *,const uint8_t *,size_t,
    const uint8_t *,size_t,struct infs20_tree_ref *);
infs_status infs20_tree_remove(const struct infs20_tree_io *,
    const struct infs20_tree_ref *,const uint8_t *,size_t,
    struct infs20_tree_ref *);
infs_status infs20_tree_get(const struct infs20_tree_io *,
    const struct infs20_tree_ref *,const uint8_t *,size_t,
    uint8_t *,size_t,size_t *);
/* Full traversal verifies all reachable pages and exact inherited key
 * ranges. A NULL visitor is a structural validation pass. A non-NULL
 * visitor may have consumed earlier values before later failure; use
 * a separate validation pass before externally publishing an export. */
infs_status infs20_tree_visit(const struct infs20_tree_io *,
    const struct infs20_tree_ref *,infs20_tree_visitor,void *);
/* Explicit maintenance traversal of verified immutable page references.
 * The visitor sees each referenced node after its digest and inherited
 * key bounds validate. No storage writes occur. */
typedef infs_status (*infs20_tree_ref_visitor)(void *,const struct infs20_tree_ref *);
infs_status infs20_tree_visit_refs(const struct infs20_tree_io *,
    const struct infs20_tree_ref *,infs20_tree_ref_visitor,void *);
#endif
