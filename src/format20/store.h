// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef INFS20_STORE_H
#define INFS20_STORE_H
#include "schema.h"

/*
 * Bounded single-device S0/S1 executable model. The bank geometry is an
 * explicit test profile, NOT a proposed production allocator. All ordinary
 * metadata and commit pages use the canonical envelope; child digests in
 * the root prove membership. The caller owns exclusive access throughout
 * each operation. No raw-device adapter or production mount is provided.
 */
#define INFS20_STORE_LEAVES 8u
#define INFS20_STORE_BANK_BLOCKS (2u + INFS20_STORE_LEAVES)
#define INFS20_STORE_LANE_BLOCKS (2u * INFS20_STORE_BANK_BLOCKS)
#define INFS20_STORE_KEY_MAX 1023u

struct infs20_store_layout {
    uint8_t set_id[16], scope_id[16], member_id[16];
    uint64_t member_blocks, lane_start[INFS20_LANE_COUNT];
};

struct infs20_store_io {
    void *context;
    infs_status (*read)(void *, uint64_t, uint8_t *);
    infs_status (*write)(void *, uint64_t, const uint8_t *);
    infs_status (*flush)(void *);
};

struct infs20_store_record {
    const uint8_t *key, *value;
    uint16_t key_size, value_size;
};

struct infs20_store_state {
    uint64_t sequence;
    uint8_t transaction_id[16], commit_digest[32], root_digest[32];
    uint16_t leaves, records;
    uint8_t fully_protected;
};

/* On failure outputs are unchanged. A blank set returns NOT_FOUND; damaged
 * or unsupported commit evidence never becomes an implicit blank format. */
infs_status infs20_store_recover(const struct infs20_store_layout *,
    const struct infs20_store_io *, struct infs20_store_state *);

/* This model replaces an entire bounded tree in a private bank. A fresh,
 * nonzero transaction ID must be provided for every attempt, including
 * retries. Never reuse a transaction ID after uncertain publication. */
infs_status infs20_store_publish(const struct infs20_store_layout *,
    const struct infs20_store_io *, const uint8_t transaction_id[16],
    const struct infs20_store_record *, size_t,
    struct infs20_store_state *);

/* Read a value only after recovering and verifying the entire bounded
 * committed tree. Caller must keep exclusive access; no concurrent mutation.
 * NULL buffer with zero capacity is allowed for an empty value. */
infs_status infs20_store_lookup(const struct infs20_store_layout *,
    const struct infs20_store_io *, const uint8_t *, size_t,
    uint8_t *, size_t, size_t *);

/* Heal copies of the uniquely selected committed generation. Never repair
 * absent authority or an unverified root. Data replicas flush before commit
 * copies; an interrupted repair is safe to resume after cold recovery. */
infs_status infs20_store_heal(const struct infs20_store_layout *,
    const struct infs20_store_io *, struct infs20_store_state *);
/* Visit borrowed record views only after complete committed-tree validation.
 * Visitor must not modify the source; failure can leave caller-owned visitor
 * state partially populated, so publish output only after success. */
typedef infs_status (*infs20_store_visitor)(void *,const struct infs20_record_view *);
infs_status infs20_store_visit(const struct infs20_store_layout *,
    const struct infs20_store_io *,infs20_store_visitor,void *);
#endif
