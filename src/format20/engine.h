// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFILFS_EXPERIMENTAL_FORMAT20_ENGINE_H
#define INFILFS_EXPERIMENTAL_FORMAT20_ENGINE_H
#include "schema.h"

/*
 * Private S0/S1 single-member prototype. It deliberately has no production
 * mount, block-device open, formatter or registered ABI. All I/O is supplied
 * by a disposable image/test backend. Fixed opaque 16-byte keys are a
 * candidate tree subset, not the eventual complete object schema.
 */
#define INFS20_ROOTS 8u
#define INFS20_REF_BYTES 72u
#define INFS20_COMMIT_BYTES (48u + INFS20_ROOTS * INFS20_REF_BYTES)
#define INFS20_TREE_HEADER 8u
#define INFS20_MAX_LEVEL 8u
#define INFS20_COMMIT_SLOTS 6u

enum infs20_root_role {
    INFS20_ROOT_ACTIVE = 0, INFS20_ROOT_RETAINED,
    INFS20_ROOT_ALLOCATIONS, INFS20_ROOT_LOCATIONS,
    INFS20_ROOT_OWNERS, INFS20_ROOT_REFERENCES,
    INFS20_ROOT_ARENA, INFS20_ROOT_RECLAIM
};

/* Addresses are single-member direct locators, in lane order. This
 * prototype commits the locator tuple as part of the parent payload.
 * Consequently metadata relocation requires rebuilding ancestor paths. */
struct infs20_ref {
    uint8_t id[16], digest[32];
    uint64_t blocks[INFS20_LANE_COUNT];
};
struct infs20_commit {
    uint64_t blocks, previous_sequence;
    uint8_t previous_digest[32];
    struct infs20_ref roots[INFS20_ROOTS];
};
struct infs20_io {
    void *context;
    infs_status (*read)(void *, uint64_t, uint8_t *);
    infs_status (*write)(void *, uint64_t, const uint8_t *);
    infs_status (*flush)(void *);
    uint64_t blocks;
    uint8_t set_id[16], member_id[16];
    /* Two independent slots per lane; writer always selects the other bank. */
    uint64_t slots[2][INFS20_LANE_COUNT];
};
struct infs20_authority {
    struct infs20_commit commit;
    uint8_t digest[32], page_id[16];
    uint64_t sequence;
    uint8_t lane_mask, bank;
};
struct infs20_tree_entry {
    uint8_t key[16];
    const uint8_t *value;
    uint16_t value_size;
};
infs_status infs20_ref_encode(uint8_t *out, const struct infs20_ref *ref,
                              uint64_t capacity);
infs_status infs20_tree_encode(uint8_t *out, size_t out_size,
                              uint16_t level, const struct infs20_tree_entry *entries,
                              size_t count, size_t *used);
infs_status infs20_commit_encode(uint8_t *out, size_t out_size,
                                const struct infs20_commit *commit,
                                uint64_t sequence);
/* Read-only graph validation, bounded by caller's page-read budget. */
infs_status infs20_verify_tree(const struct infs20_io *io,
                               const struct infs20_ref *root,
                               uint64_t sequence, size_t *budget);
/* Selection never heals, never falls back silently past a valid newer
 * commit whose graph is damaged, and refuses conflicting same-sequence roots. */
infs_status infs20_select(const struct infs20_io *io, size_t budget,
                          struct infs20_authority *out);
/* Dependency writer belongs to caller. It must reserve fresh pages and
 * retain current dependencies. This function flushes, verifies, then
 * publishes the other bank; after any error caller must cold-select before
 * mutation. Acknowledgement requires all three commit copies durable. */
infs_status infs20_publish(const struct infs20_io *io,
                           const struct infs20_authority *previous,
                           const struct infs20_commit *next,
                           const uint8_t page_id[16], size_t budget,
                           struct infs20_authority *out);
#endif
