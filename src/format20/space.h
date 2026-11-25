// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_EXPERIMENTAL_FORMAT20_SPACE_H
#define INFILFS_EXPERIMENTAL_FORMAT20_SPACE_H
#include "schema.h"

/*
 * Bounded executable reference model for S1/S2. These small limits allow
 * exhaustive fault/state testing; they are NOT production filesystem
 * limits or a proposed in-memory allocation strategy. State is copied
 * before each operation, and committed views must remain immutable.
 *
 * A physical range is reusable only after all strong references disappear
 * and the oldest pinned publication advances past its retirement sequence.
 * The registry itself is weak. No namespace traversal is needed to discover
 * reference owners. This model covers data allocations, not metadata GC.
 */
#define INFS20_MODEL_ALLOCS 24u
#define INFS20_MODEL_EDGES 64u
#define INFS20_MODEL_RETIRED 32u
#define INFS20_MODEL_ARENAS 3u

enum infs20_alloc_state {
    INFS20_ALLOC_LIVE = 1, INFS20_ALLOC_PENDING = 2
};
struct infs20_model_range {
    uint64_t start, length;
};
struct infs20_model_alloc {
    uint8_t id[16], digest[32];
    uint64_t decoded_bytes;
    struct infs20_model_range physical;
    uint64_t retire_sequence;
    uint32_t references;
    uint8_t state;
};
struct infs20_model_edge {
    uint8_t source[16], target[16];
    uint32_t ordinal;
};
struct infs20_model_retired {
    struct infs20_model_range physical;
    uint64_t sequence;
};
struct infs20_model_arena {
    struct infs20_model_range physical;
    uint64_t frontier;
};
struct infs20_space {
    uint64_t capacity, sequence;
    /* Oldest readable/pinned generation, supplied by the model's authority.
     * Retirement at sequence N is safe only when oldest_pin > N-1. */
    uint64_t oldest_pin;
    uint64_t reserved_prefix;
    size_t allocations, edges, retired;
    struct infs20_model_alloc alloc[INFS20_MODEL_ALLOCS];
    struct infs20_model_edge edge[INFS20_MODEL_EDGES];
    struct infs20_model_retired old[INFS20_MODEL_RETIRED];
    struct infs20_model_arena arena[INFS20_MODEL_ARENAS];
};

infs_status infs20_space_init(struct infs20_space *s,uint64_t capacity,
                              uint64_t reserved_prefix,
                              const struct infs20_model_range arenas[3]);
infs_status infs20_space_validate(const struct infs20_space *s);
/* Prepare a private successor. The caller may publish it only with the
 * matching committed roots; discarding it never mutates the source. */
infs_status infs20_space_begin(const struct infs20_space *current,
                               struct infs20_space *next);
infs_status infs20_space_add(struct infs20_space *s,const uint8_t id[16],
                             const uint8_t digest[32],uint64_t decoded_bytes,
                             struct infs20_model_range physical,
                             const uint8_t source[16],uint32_t ordinal);
infs_status infs20_space_link(struct infs20_space *s,const uint8_t target[16],
                              const uint8_t source[16],uint32_t ordinal);
infs_status infs20_space_unlink(struct infs20_space *s,const uint8_t source[16],
                                uint32_t ordinal);
/* Caller has already verified the equivalent content at the new location.
 * The former range is retained independently of the current descriptor. */
infs_status infs20_space_relocate(struct infs20_space *s,const uint8_t target[16],
                                  struct infs20_model_range physical);
/* Advance only with authoritative proof that no older reader/fallback/root
 * can address the physical versions. Not a production pin-discovery API. */
infs_status infs20_space_advance_pin(struct infs20_space *s,uint64_t oldest);
infs_status infs20_space_collect(struct infs20_space *s,size_t limit,
                                 size_t *released);
/* Atomically reserve equivalent append space in all three fixed arenas.
 * No reuse/evacuation in this candidate model. */
infs_status infs20_space_arena_reserve(struct infs20_space *s,uint64_t blocks,
                                       uint64_t starts[3]);
#endif
