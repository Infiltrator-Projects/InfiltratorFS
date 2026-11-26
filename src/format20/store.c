// SPDX-License-Identifier: GPL-3.0-or-later
#include "store.h"
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include <stdlib.h>
#include <string.h>

/*
 * Experimental payload grammar. The root commits the ordered leaf digests;
 * the commit commits the root, exact layout, transaction and predecessor.
 * There is no independent backup namespace or last-writer-wins leaf scan.
 * Physical copies use identical logical payloads and distinct envelope
 * placement. Bank selection is sequence parity; never overwrite the
 * selected authority's bank while preparing its successor.
 */
enum { COMMIT_BYTES = 124, ROOT_HEADER = 8, LEAF_HEADER = 8 };
struct tree_image {
    uint8_t root[INFS20_PAYLOAD_BYTES];
    uint8_t leaf[INFS20_STORE_LEAVES][INFS20_PAYLOAD_BYTES];
    uint32_t root_size, leaf_size[INFS20_STORE_LEAVES];
    uint16_t leaves, records;
};
struct commit_candidate {
    struct infs20_store_state state;
    uint8_t payload[COMMIT_BYTES];
};
static int zero(const uint8_t *p, size_t n)
{
    uint8_t v = 0;
    while (n--) v |= *p++;
    return v == 0;
}
static int keycmp(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    size_t n = an < bn ? an : bn;
    int c = memcmp(a, b, n);
    return c ? c : (an > bn) - (an < bn);
}
static int layout_valid(const struct infs20_store_layout *l,
                        const struct infs20_store_io *io, int writable)
{
    if (!l || !io || !io->read || (writable && (!io->write || !io->flush)) ||
        zero(l->set_id, 16) || zero(l->scope_id, 16) ||
        zero(l->member_id, 16) ||
        l->member_blocks > INFS20_MAX_MEMBER_BLOCKS ||
        l->member_blocks < INFS20_STORE_LANE_BLOCKS)
        return 0;
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i) {
        uint64_t a = l->lane_start[i];
        if (a > l->member_blocks - INFS20_STORE_LANE_BLOCKS) return 0;
        for (unsigned j = 0; j < i; ++j) {
            uint64_t b = l->lane_start[j];
            if (a < b + INFS20_STORE_LANE_BLOCKS &&
                b < a + INFS20_STORE_LANE_BLOCKS) return 0;
        }
    }
    return 1;
}
static uint64_t address(const struct infs20_store_layout *l, unsigned lane,
                        uint64_t seq, unsigned slot)
{
    return l->lane_start[lane] + (seq & 1u) * INFS20_STORE_BANK_BLOCKS + slot;
}
static void identity(const struct infs20_store_layout *l, uint64_t seq,
                     const uint8_t tx[16], unsigned slot, unsigned lane,
                     struct infs20_page_identity *id)
{
    uint8_t preimage[25] = {'I','2','0','I','D',0,0,0,0};
    uint8_t digest[32];
    infs_store_le32(preimage + 5, slot);
    memcpy(preimage + 9, tx, 16);
    infs_sha256(preimage, sizeof(preimage), digest);
    memset(id, 0, sizeof(*id));
    memcpy(id->page_id, digest, 16);
    memcpy(id->set_id, l->set_id, 16);
    memcpy(id->scope_id, l->scope_id, 16);
    memcpy(id->member_id, l->member_id, 16);
    id->sequence = seq;
    id->block = address(l, lane, seq, slot);
    id->lane = (uint16_t)lane;
}
static infs_status encode(const struct infs20_store_layout *l,
                          const struct infs20_store_state *s, unsigned slot,
                          unsigned lane, const uint8_t *payload, size_t n,
                          uint8_t page[INFS20_PAGE_BYTES])
{
    struct infs20_page_identity id;
    identity(l, s->sequence, s->transaction_id, slot, lane, &id);
    return infs20_page_encode(page, INFS20_PAGE_BYTES, &id, payload, n);
}
static int matches(const struct infs20_decoded_page *p,
                   const struct infs20_page_identity *id)
{
    return p->identity.sequence == id->sequence &&
           p->identity.lane == id->lane &&
           !memcmp(p->identity.set_id, id->set_id, 16) &&
           !memcmp(p->identity.scope_id, id->scope_id, 16) &&
           !memcmp(p->identity.page_id, id->page_id, 16);
}
static infs_status read_copy(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const struct infs20_store_state *s,
    unsigned slot, unsigned lane, const uint8_t digest[32],
    uint8_t page[INFS20_PAGE_BYTES], struct infs20_decoded_page *d)
{
    struct infs20_page_identity id;
    identity(l, s->sequence, s->transaction_id, slot, lane, &id);
    infs_status st = io->read(io->context, id.block, page);
    if (st) return st;
    st = infs20_page_decode(page, INFS20_PAGE_BYTES, l->member_id,
                           id.block, l->member_blocks, d);
    if (st) return st;
    return matches(d, &id) && !memcmp(d->digest, digest, 32) ?
           INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}
/* Copy absence/corruption is tolerable when another independently verified
 * replica satisfies the same committed reference. Operational errors are
 * not converted into corruption if no usable copy survives. */
static infs_status read_node(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const struct infs20_store_state *s,
    unsigned slot, const uint8_t digest[32], uint8_t *payload,
    uint32_t *size, uint8_t *healthy)
{
    uint8_t page[INFS20_PAGE_BYTES];
    struct infs20_decoded_page d;
    unsigned count = 0;
    infs_status operational = INFS_STATUS_OK;
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        infs_status st = read_copy(l, io, s, slot, lane, digest, page, &d);
        if (!st) {
            if (!count) {
                *size = d.payload_size;
                memcpy(payload, page + INFS20_OFF_PAYLOAD, *size);
            }
            ++count;
        } else if (st != INFS_STATUS_CORRUPT &&
                   st != INFS_STATUS_NOT_SUPPORTED) operational = st;
    }
    if (!count) return operational ? operational : INFS_STATUS_CORRUPT;
    if (count != INFS20_LANE_COUNT) *healthy = 0;
    return INFS_STATUS_OK;
}
static infs_status decode_commit(const struct infs20_store_layout *l,
    const uint8_t *page, unsigned lane, unsigned bank,
    struct commit_candidate *c)
{
    struct infs20_decoded_page d;
    uint64_t block = address(l, lane, bank, 0);
    infs_status st = infs20_page_decode(page, INFS20_PAGE_BYTES,
                              l->member_id, block, l->member_blocks, &d);
    if (st) return st;
    const uint8_t *p = page + INFS20_OFF_PAYLOAD;
    if (d.payload_size != COMMIT_BYTES || memcmp(p, "C20!", 4) ||
        (d.identity.sequence & 1u) != bank ||
        zero(p + 4, 16) || infs_load_le64(p + 20) != l->member_blocks ||
        zero(p + 52, 32) ||
        (d.identity.sequence == 1 ? !zero(p + 84, 32) : zero(p + 84, 32)) ||
        !zero(p + 120, 4))
        return INFS_STATUS_CORRUPT;
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i)
        if (infs_load_le64(p + 28 + i * 8u) != l->lane_start[i])
            return INFS_STATUS_CORRUPT;
    struct infs20_page_identity id;
    identity(l, d.identity.sequence, p + 4, 0, lane, &id);
    if (!matches(&d, &id)) return INFS_STATUS_CORRUPT;
    memset(c, 0, sizeof(*c));
    c->state.sequence = d.identity.sequence;
    memcpy(c->state.transaction_id, p + 4, 16);
    memcpy(c->state.root_digest, p + 52, 32);
    memcpy(c->state.commit_digest, d.digest, 32);
    c->state.leaves = infs_load_le16(p + 116);
    c->state.records = infs_load_le16(p + 118);
    if (c->state.leaves > INFS20_STORE_LEAVES ||
        (!!c->state.leaves != !!c->state.records))
        return INFS_STATUS_CORRUPT;
    memcpy(c->payload, p, COMMIT_BYTES);
    return INFS_STATUS_OK;
}
static infs_status select_commit(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, struct commit_candidate *out)
{
    struct commit_candidate candidates[2 * INFS20_LANE_COUNT];
    unsigned count = 0;
    int damaged = 0;
    uint8_t page[INFS20_PAGE_BYTES];
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        for (unsigned bank = 0; bank < 2; ++bank) {
            infs_status st = io->read(io->context, address(l, lane, bank, 0), page);
            if (st) return st; /* cannot exclude newer authority */
            if (zero(page, sizeof(page))) continue;
            st = decode_commit(l, page, lane, bank, &candidates[count]);
            if (st) {
                /* Recognizable unsupported envelope must not be treated as
                 * an older valid state. Random/torn bytes are damage. */
                if (st == INFS_STATUS_NOT_SUPPORTED &&
                    !memcmp(page, "INF20S0!", 8)) return st;
                damaged = 1;
                continue;
            }
            ++count;
        }
    }
    if (!count) return damaged ? INFS_STATUS_CORRUPT : INFS_STATUS_NOT_FOUND;
    unsigned newest = 0;
    for (unsigned i = 1; i < count; ++i)
        if (candidates[i].state.sequence > candidates[newest].state.sequence)
            newest = i;
    unsigned copies = 0;
    for (unsigned i = 0; i < count; ++i) {
        if (candidates[i].state.sequence != candidates[newest].state.sequence)
            continue;
        if (memcmp(candidates[i].state.commit_digest,
                   candidates[newest].state.commit_digest, 32))
            return INFS_STATUS_CORRUPT;
        ++copies;
    }
    /* If a valid immediately preceding commit survives, the chain must
     * agree. Absence of a predecessor is allowed after bank/lane loss. */
    for (unsigned i = 0; i < count; ++i)
        if (candidates[newest].state.sequence > 1 &&
            candidates[i].state.sequence == candidates[newest].state.sequence - 1 &&
            memcmp(candidates[i].state.commit_digest,
                   candidates[newest].payload + 84, 32))
            return INFS_STATUS_CORRUPT;
    *out = candidates[newest];
    out->state.fully_protected = copies == INFS20_LANE_COUNT;
    return INFS_STATUS_OK;
}
static infs_status validate_tree(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, struct commit_candidate *c,
    struct tree_image *t)
{
    struct infs20_store_state *s = &c->state;
    infs_status st = read_node(l, io, s, 1, s->root_digest, t->root,
                               &t->root_size, &s->fully_protected);
    if (st) return st;
    if (t->root_size != ROOT_HEADER + (uint32_t)s->leaves * 32u ||
        memcmp(t->root, "T20R", 4) ||
        infs_load_le16(t->root + 4) != s->leaves ||
        infs_load_le16(t->root + 6) != s->records)
        return INFS_STATUS_CORRUPT;
    t->leaves = s->leaves;
    t->records = s->records;
    const uint8_t *previous = NULL;
    size_t previous_size = 0;
    unsigned total = 0;
    for (unsigned i = 0; i < s->leaves; ++i) {
        uint8_t *leaf = t->leaf[i];
        st = read_node(l, io, s, i + 2, t->root + ROOT_HEADER + 32u*i,
                       leaf, &t->leaf_size[i], &s->fully_protected);
        if (st) return st;
        if (t->leaf_size[i] < LEAF_HEADER || memcmp(leaf, "T20L", 4) ||
            !zero(leaf + 6, 2)) return INFS_STATUS_CORRUPT;
        unsigned n = infs_load_le16(leaf + 4);
        if (!n) return INFS_STATUS_CORRUPT;
        size_t offset = LEAF_HEADER;
        for (unsigned j = 0; j < n; ++j) {
            struct infs20_record_view r;
            if (offset >= t->leaf_size[i]) return INFS_STATUS_CORRUPT;
            st = infs20_record_decode(leaf + offset,
                                      t->leaf_size[i] - offset, &r);
            if (st) return st;
            if (r.key_size > INFS20_STORE_KEY_MAX ||
                (previous && keycmp(previous, previous_size,
                                    r.key, r.key_size) >= 0))
                return INFS_STATUS_CORRUPT;
            previous = r.key;
            previous_size = r.key_size;
            offset += r.consumed;
            ++total;
        }
        if (offset != t->leaf_size[i]) return INFS_STATUS_CORRUPT;
    }
    return total == s->records ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}
static infs_status load(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, struct commit_candidate *c,
    struct tree_image *t)
{
    infs_status st = select_commit(l, io, c);
    return st ? st : validate_tree(l, io, c, t);
}
infs_status infs20_store_recover(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, struct infs20_store_state *out)
{
    if (!out || !layout_valid(l, io, 0)) return INFS_STATUS_INVALID_ARGUMENT;
    struct tree_image *t = calloc(1, sizeof(*t));
    if (!t) return INFS_STATUS_NO_MEMORY;
    struct commit_candidate c;
    infs_status st = load(l, io, &c, t);
    if (!st) *out = c.state;
    free(t);
    return st;
}
static infs_status build_tree(const struct infs20_store_record *r, size_t n,
                              struct tree_image *t)
{
    if (n > UINT16_MAX || (n && !r)) return INFS_STATUS_INVALID_ARGUMENT;
    for (size_t i = 0; i < n; ++i) {
        if (!r[i].key || !r[i].key_size || r[i].key_size > INFS20_STORE_KEY_MAX ||
            (r[i].value_size && !r[i].value) ||
            (i && keycmp(r[i-1].key, r[i-1].key_size,
                         r[i].key, r[i].key_size) >= 0))
            return INFS_STATUS_INVALID_ARGUMENT;
        size_t bytes = INFS20_RECORD_HEADER_BYTES + (size_t)r[i].key_size +
                       r[i].value_size;
        if (bytes > INFS20_PAYLOAD_BYTES - LEAF_HEADER) return INFS_STATUS_NO_SPACE;
        if (!t->leaves || bytes > INFS20_PAYLOAD_BYTES - t->leaf_size[t->leaves-1]) {
            if (t->leaves == INFS20_STORE_LEAVES) return INFS_STATUS_NO_SPACE;
            memcpy(t->leaf[t->leaves], "T20L", 4);
            t->leaf_size[t->leaves++] = LEAF_HEADER;
        }
        unsigned leaf = t->leaves - 1;
        uint8_t *p = t->leaf[leaf] + t->leaf_size[leaf];
        infs_store_le16(p, INFS20_PROBE_KIND);
        infs_store_le16(p + 4, r[i].key_size);
        infs_store_le16(p + 6, r[i].value_size);
        memcpy(p + 8, r[i].key, r[i].key_size);
        if (r[i].value_size) memcpy(p + 8 + r[i].key_size, r[i].value, r[i].value_size);
        t->leaf_size[leaf] += (uint32_t)bytes;
        infs_store_le16(t->leaf[leaf] + 4,
            infs_load_le16(t->leaf[leaf] + 4) + 1u);
    }
    t->records = (uint16_t)n;
    memcpy(t->root, "T20R", 4);
    infs_store_le16(t->root + 4, t->leaves);
    infs_store_le16(t->root + 6, t->records);
    t->root_size = ROOT_HEADER + 32u*t->leaves;
    return INFS_STATUS_OK;
}
static infs_status write_node(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const struct infs20_store_state *s,
    unsigned slot, unsigned lane, const uint8_t *payload, size_t size)
{
    uint8_t page[INFS20_PAGE_BYTES];
    infs_status st = encode(l, s, slot, lane, payload, size, page);
    return st ? st : io->write(io->context, address(l, lane, s->sequence, slot), page);
}
infs_status infs20_store_publish_verified(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const uint8_t tx[16],
    const struct infs20_store_record *records, size_t n,
    infs20_store_prepare prepare, void *prepare_context,
    infs20_store_prepare verify, void *verify_context,
    struct infs20_store_state *out)
{
    if (!out || !tx || zero(tx, 16) || !layout_valid(l, io, 1))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct tree_image *t = calloc(1, sizeof(*t));
    if (!t) return INFS_STATUS_NO_MEMORY;
    infs_status st = build_tree(records, n, t);
    if (st) { free(t); return st; }
    struct infs20_store_state old = {0}, next = {0};
    st = infs20_store_recover(l, io, &old);
    if (st != INFS_STATUS_NOT_FOUND && st) goto done;
    if ((!st && !old.fully_protected) ||
        (!st && !memcmp(old.transaction_id, tx, 16))) {
        st = INFS_STATUS_BUSY;
        goto done;
    }
    if (old.sequence == UINT64_MAX) { st = INFS_STATUS_OVERFLOW; goto done; }
    next.sequence = old.sequence + 1;
    memcpy(next.transaction_id, tx, 16);
    next.leaves = t->leaves;
    next.records = t->records;
    uint8_t page[INFS20_PAGE_BYTES], commit[COMMIT_BYTES] = {0};
    /* Construct every logical dependency and commitment before first write.
     * This proves the bounded bank reservation fits, not general ENOSPC. */
    for (unsigned i = 0; i < t->leaves; ++i) {
        st = encode(l, &next, i+2, 0, t->leaf[i], t->leaf_size[i], page);
        if (st) goto done;
        memcpy(t->root + ROOT_HEADER + 32u*i, page + INFS20_OFF_DIGEST, 32);
    }
    st = encode(l, &next, 1, 0, t->root, t->root_size, page);
    if (st) goto done;
    memcpy(next.root_digest, page + INFS20_OFF_DIGEST, 32);
    memcpy(commit, "C20!", 4);
    memcpy(commit + 4, tx, 16);
    infs_store_le64(commit + 20, l->member_blocks);
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i)
        infs_store_le64(commit + 28 + 8u*i, l->lane_start[i]);
    memcpy(commit + 52, next.root_digest, 32);
    memcpy(commit + 84, old.commit_digest, 32);
    infs_store_le16(commit + 116, t->leaves);
    infs_store_le16(commit + 118, t->records);
    st = encode(l, &next, 0, 0, commit, sizeof(commit), page);
    if (st) goto done;
    memcpy(next.commit_digest, page + INFS20_OFF_DIGEST, 32);
    if (prepare) {
        st = prepare(prepare_context);
        if (st) goto done;
    }
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        for (unsigned i = 0; i < t->leaves; ++i) {
            st = write_node(l, io, &next, i+2, lane, t->leaf[i], t->leaf_size[i]);
            if (st) goto done;
        }
        st = write_node(l, io, &next, 1, lane, t->root, t->root_size);
        if (st) goto done;
    }
    st = io->flush(io->context);
    if (st) goto done;
    /* Verify every declared dependency from the storage backend, not the
     * private staging buffers. A successful write/flush return is not proof
     * that the correct page was written to every address. No new authority
     * may be published with a missing or silently corrupted metadata copy. */
    struct infs20_decoded_page decoded;
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        for (unsigned i = 0; i < t->leaves; ++i) {
            st = read_copy(l, io, &next, i+2, lane,
                           t->root + ROOT_HEADER + 32u*i, page, &decoded);
            if (st) goto done;
        }
        st = read_copy(l, io, &next, 1, lane, next.root_digest, page, &decoded);
        if (st) goto done;
    }
    if (verify) { st = verify(verify_context); if (st) goto done; }
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        st = write_node(l, io, &next, 0, lane, commit, sizeof(commit));
        if (st) goto done;
        st = io->flush(io->context);
        if (st) goto done;
        st = read_copy(l, io, &next, 0, lane, next.commit_digest, page, &decoded);
        if (st) goto done;
    }
    next.fully_protected = 1;
    *out = next;
done:
    free(t);
    return st;
}
infs_status infs20_store_publish_prepared(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const uint8_t tx[16],
    const struct infs20_store_record *records, size_t n,
    infs20_store_prepare prepare, void *prepare_context,
    struct infs20_store_state *out)
{
    return infs20_store_publish_verified(l,io,tx,records,n,prepare,prepare_context,
                                          NULL,NULL,out);
}
infs_status infs20_store_publish(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const uint8_t tx[16],
    const struct infs20_store_record *records, size_t n,
    struct infs20_store_state *out)
{
    return infs20_store_publish_prepared(l,io,tx,records,n,NULL,NULL,out);
}
infs_status infs20_store_lookup(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, const uint8_t *key, size_t key_size,
    uint8_t *out, size_t capacity, size_t *size)
{
    if (!key || !key_size || key_size > INFS20_STORE_KEY_MAX ||
        !size || (!out && capacity) || !layout_valid(l, io, 0))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct tree_image *t = calloc(1, sizeof(*t));
    if (!t) return INFS_STATUS_NO_MEMORY;
    struct commit_candidate c;
    infs_status st = load(l, io, &c, t);
    if (st) goto done;
    st = INFS_STATUS_NOT_FOUND;
    for (unsigned i = 0; i < t->leaves; ++i) {
        size_t offset = LEAF_HEADER;
        while (offset < t->leaf_size[i]) {
            struct infs20_record_view r;
            st = infs20_record_decode(t->leaf[i]+offset, t->leaf_size[i]-offset, &r);
            if (st) goto done;
            if (!keycmp(key, key_size, r.key, r.key_size)) {
                if (r.value_size > capacity) { st = INFS_STATUS_NO_SPACE; goto done; }
                if (r.value_size) memcpy(out, r.value, r.value_size);
                *size = r.value_size;
                st = INFS_STATUS_OK;
                goto done;
            }
            offset += r.consumed;
            st = INFS_STATUS_NOT_FOUND;
        }
    }
done:
    free(t);
    return st;
}
infs_status infs20_store_heal(const struct infs20_store_layout *l,
    const struct infs20_store_io *io, struct infs20_store_state *out)
{
    if (!out || !layout_valid(l, io, 1)) return INFS_STATUS_INVALID_ARGUMENT;
    struct tree_image *t = calloc(1, sizeof(*t));
    if (!t) return INFS_STATUS_NO_MEMORY;
    struct commit_candidate c;
    infs_status st = load(l, io, &c, t);
    if (st) goto done;
    if (c.state.fully_protected) { *out = c.state; goto done; }
    /* Only write missing copies; never rewrite the last surviving proof.
     * All replacement pages are verified against the existing commitment
     * before they can be published by the same commit record. */
    uint8_t page[INFS20_PAGE_BYTES];
    struct infs20_decoded_page d;
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        for (unsigned slot = 1; slot < (unsigned)t->leaves + 2u; ++slot) {
            const uint8_t *digest = slot == 1 ? c.state.root_digest :
                t->root + ROOT_HEADER + 32u*(slot-2);
            const uint8_t *payload = slot == 1 ? t->root : t->leaf[slot-2];
            uint32_t length = slot == 1 ? t->root_size : t->leaf_size[slot-2];
            st = read_copy(l, io, &c.state, slot, lane, digest, page, &d);
            if (!st) continue;
            if (st != INFS_STATUS_CORRUPT && st != INFS_STATUS_NOT_SUPPORTED)
                goto done;
            st = encode(l, &c.state, slot, lane, payload, length, page);
            if (st) goto done;
            if (memcmp(page + INFS20_OFF_DIGEST, digest, 32)) {
                st = INFS_STATUS_CORRUPT; goto done;
            }
            st = io->write(io->context, address(l, lane, c.state.sequence, slot), page);
            if (st) goto done;
        }
    }
    st = io->flush(io->context);
    if (st) goto done;
    for (unsigned lane = 0; lane < INFS20_LANE_COUNT; ++lane) {
        st = read_copy(l, io, &c.state, 0, lane, c.state.commit_digest, page, &d);
        if (!st) continue;
        if (st != INFS_STATUS_CORRUPT && st != INFS_STATUS_NOT_SUPPORTED) goto done;
        st = encode(l, &c.state, 0, lane, c.payload, sizeof(c.payload), page);
        if (st) goto done;
        if (memcmp(page + INFS20_OFF_DIGEST, c.state.commit_digest, 32)) {
            st = INFS_STATUS_CORRUPT; goto done;
        }
        st = io->write(io->context, address(l, lane, c.state.sequence, 0), page);
        if (st) goto done;
        st = io->flush(io->context);
        if (st) goto done;
    }
    /* Re-read rather than asserting health from attempted writes. */
    st = load(l, io, &c, t);
    if (!st && !c.state.fully_protected) st = INFS_STATUS_CORRUPT;
    if (!st) *out = c.state;
done:
    free(t);
    return st;
}

infs_status infs20_store_visit(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,infs20_store_visitor visit,void *context)
{
    if(!visit || !layout_valid(l,io,0)) return INFS_STATUS_INVALID_ARGUMENT;
    struct tree_image *t=calloc(1,sizeof(*t));
    if(!t) return INFS_STATUS_NO_MEMORY;
    struct commit_candidate c;
    infs_status st=load(l,io,&c,t);
    if(st) goto done;
    for(unsigned i=0;i<t->leaves;++i) {
        size_t offset=LEAF_HEADER;
        while(offset<t->leaf_size[i]) {
            struct infs20_record_view r;
            st=infs20_record_decode(t->leaf[i]+offset,t->leaf_size[i]-offset,&r);
            if(st) goto done;
            st=visit(context,&r);
            if(st) goto done;
            offset+=r.consumed;
        }
    }
done:
    free(t);
    return st;
}

/*
 * Raw offline discovery for the experimental commit profile. The envelope
 * supplies a candidate relative address and member identity; the observed
 * physical offset supplies the original start. A complete committed root
 * must then validate through independently bounded region reads. Merely
 * recognizing a metadata leaf or CRC never establishes a filesystem.
 */
static infs_status region_read(void *opaque,uint64_t block,uint8_t *page)
{
    struct infs20_store_region *r=opaque;
    if(!r || !page || !r->source.read_at ||
       r->start>r->source.bytes || block>=r->layout.member_blocks ||
       block>INFS20_MAX_BLOCK)
        return INFS_STATUS_INVALID_ARGUMENT;
    uint64_t available=r->source.bytes-r->start;
    uint64_t relative=block*UINT64_C(4096);
    if(relative>available || INFS20_PAGE_BYTES>available-relative)
        return INFS_STATUS_IO_ERROR;
    return r->source.read_at(r->source.context,r->start+relative,
                             page,INFS20_PAGE_BYTES);
}
struct infs20_store_io infs20_store_region_io(struct infs20_store_region *r)
{
    struct infs20_store_io io={r,region_read,NULL,NULL};
    return io;
}
static int same_region(const struct infs20_store_region *a,
                       const struct infs20_store_region *b)
{
    return a->start==b->start &&
           a->layout.member_blocks==b->layout.member_blocks &&
           !memcmp(a->layout.set_id,b->layout.set_id,16) &&
           !memcmp(a->layout.scope_id,b->layout.scope_id,16) &&
           !memcmp(a->layout.member_id,b->layout.member_id,16) &&
           !memcmp(a->layout.lane_start,b->layout.lane_start,
                   sizeof(a->layout.lane_start));
}
static infs_status probe_region(const struct infs20_raw_source *source,
    uint64_t observed,const uint8_t *page,struct infs20_store_region *out)
{
    if(memcmp(page,"INF20S0!",8) ||
       memcmp(page+INFS20_OFF_PAYLOAD,"C20!",4))
        return INFS_STATUS_NOT_FOUND;
    const uint8_t *p=page+INFS20_OFF_PAYLOAD;
    struct infs20_store_region r={0};
    r.source=*source;
    r.layout.member_blocks=infs_load_le64(p+20);
    uint64_t block=infs_load_le64(page+INFS20_OFF_BLOCK);
    if(block>INFS20_MAX_BLOCK || !r.layout.member_blocks ||
       r.layout.member_blocks>INFS20_MAX_MEMBER_BLOCKS ||
       block>=r.layout.member_blocks)
        return INFS_STATUS_CORRUPT;
    uint64_t relative=block*UINT64_C(4096);
    if(relative>observed) return INFS_STATUS_CORRUPT;
    r.start=observed-relative;
    if(r.start%512u || r.start>source->bytes ||
       r.layout.member_blocks*UINT64_C(4096)>source->bytes-r.start)
        return INFS_STATUS_CORRUPT;
    memcpy(r.layout.set_id,page+INFS20_OFF_SET,16);
    memcpy(r.layout.scope_id,page+INFS20_OFF_SCOPE,16);
    memcpy(r.layout.member_id,page+INFS20_OFF_MEMBER,16);
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i)
        r.layout.lane_start[i]=infs_load_le64(p+28+8u*i);
    struct infs20_store_io io=infs20_store_region_io(&r);
    if(!layout_valid(&r.layout,&io,0)) return INFS_STATUS_CORRUPT;
    unsigned lane=infs_load_le16(page+INFS20_OFF_LANE);
    uint64_t seq=infs_load_le64(page+INFS20_OFF_SEQUENCE);
    unsigned bank=(unsigned)(seq&1u);
    if(lane>=INFS20_LANE_COUNT || block!=address(&r.layout,lane,bank,0))
        return INFS_STATUS_CORRUPT;
    struct commit_candidate c;
    infs_status st=decode_commit(&r.layout,page,lane,bank,&c);
    if(st) return st;
    *out=r;
    return INFS_STATUS_OK;
}
infs_status infs20_store_discover(const struct infs20_raw_source *source,
                                 struct infs20_store_discovery *out)
{
    if(!source || !source->read_at || !out) return INFS_STATUS_INVALID_ARGUMENT;
    if(source->bytes<INFS20_PAGE_BYTES) return INFS_STATUS_NOT_FOUND;
    struct infs20_store_discovery result={0};
    uint8_t page[INFS20_PAGE_BYTES];
    int found=0,damaged=0;
    /* A 512-byte stride supports legacy partition alignment. This prototype
     * intentionally favors bounded memory and simple proof over scan speed.
     * Production recovery should batch reads and expose scan progress. */
    for(uint64_t at=0;at<=source->bytes-INFS20_PAGE_BYTES;at+=512u) {
        infs_status st=source->read_at(source->context,at,page,sizeof(page));
        if(st) return st;
        struct infs20_store_region candidate;
        st=probe_region(source,at,page,&candidate);
        if(st==INFS_STATUS_NOT_FOUND) continue;
        if(st) {
            /* Unsupported recognizable commits are not silently treated as
             * damage to a known format. Ordinary torn copies may be skipped. */
            if(st==INFS_STATUS_NOT_SUPPORTED) return st;
            damaged=1;
            continue;
        }
        if(found && !same_region(&result.region,&candidate))
            return INFS_STATUS_CORRUPT;
        result.region=candidate;
        found=1;
    }
    if(!found) return damaged?INFS_STATUS_CORRUPT:INFS_STATUS_NOT_FOUND;
    struct infs20_store_io io=infs20_store_region_io(&result.region);
    infs_status st=infs20_store_recover(&result.region.layout,&io,&result.state);
    /* A candidate commit with missing proof cannot become an older
     * guessed namespace. The existing selector/validator owns authority. */
    if(!st) *out=result;
    return st;
}

/* Bounded normal discovery for a known fixed-lane candidate profile.
 * A lane hint is never a commit authority. Both bank headers are inspected
 * at their exact expected addresses, and the common selector must prove
 * membership and reject divergent commits. No scan, geometry inference
 * from an unvalidated page, displaced-volume fallback, or storage writes. */
infs_status infs20_store_open_bounded(const struct infs20_raw_source *source,
    const uint64_t lane_start[INFS20_LANE_COUNT],struct infs20_store_discovery *out)
{
    if(!source || !source->read_at || !lane_start || !out ||
       source->bytes%INFS20_PAGE_BYTES)
        return INFS_STATUS_INVALID_ARGUMENT;
    const uint64_t blocks=source->bytes/INFS20_PAGE_BYTES;
    if(!blocks || blocks>INFS20_MAX_MEMBER_BLOCKS)
        return INFS_STATUS_INVALID_ARGUMENT;
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i) {
        if(lane_start[i]>=blocks ||
           INFS20_STORE_LANE_BLOCKS>blocks-lane_start[i])
            return INFS_STATUS_INVALID_ARGUMENT;
        for(unsigned j=0;j<i;++j)
            if(lane_start[i]<lane_start[j]+INFS20_STORE_LANE_BLOCKS &&
               lane_start[j]<lane_start[i]+INFS20_STORE_LANE_BLOCKS)
                return INFS_STATUS_INVALID_ARGUMENT;
    }
    struct infs20_store_discovery result={0};
    uint8_t page[INFS20_PAGE_BYTES];
    int found=0,damaged=0;
    infs_status io_error=INFS_STATUS_OK;
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i) {
        for(unsigned bank=0;bank<2u;++bank) {
            const uint64_t block=lane_start[i]+bank*INFS20_STORE_BANK_BLOCKS;
            const uint64_t at=block*INFS20_PAGE_BYTES;
            infs_status st=source->read_at(source->context,at,page,sizeof(page));
            if(st) {
                if(st!=INFS_STATUS_IO_ERROR)return st;
                io_error=st;
                continue;
            }
            struct infs20_store_region candidate;
            st=probe_region(source,at,page,&candidate);
            if(st==INFS_STATUS_NOT_FOUND)continue;
            if(st==INFS_STATUS_NOT_SUPPORTED)return st;
            if(st){damaged=1;continue;}
            if(candidate.start || candidate.layout.member_blocks!=blocks ||
               memcmp(candidate.layout.lane_start,lane_start,
                      sizeof(candidate.layout.lane_start)))
                return INFS_STATUS_CORRUPT;
            if(found && !same_region(&result.region,&candidate))
                return INFS_STATUS_CORRUPT;
            result.region=candidate;
            found=1;
        }
    }
    if(!found)return damaged?INFS_STATUS_CORRUPT:
        io_error?io_error:INFS_STATUS_NOT_FOUND;
    struct infs20_store_io io=infs20_store_region_io(&result.region);
    infs_status st=infs20_store_recover(&result.region.layout,&io,&result.state);
    if(!st)*out=result;
    return st;
}
