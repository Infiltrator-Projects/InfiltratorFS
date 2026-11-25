// SPDX-License-Identifier: GPL-3.0-or-later
#include "engine.h"
#include "infilfs/endian.h"
#include <string.h>

static int nonzero(const uint8_t *p, size_t n)
{
    uint8_t v = 0;
    while (n--) v |= *p++;
    return v != 0;
}
static int ref_valid(const struct infs20_ref *r, uint64_t capacity)
{
    if (!r || !capacity || capacity > INFS20_MAX_MEMBER_BLOCKS ||
        !nonzero(r->id, 16) || !nonzero(r->digest, 32))
        return 0;
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i) {
        if (r->blocks[i] >= capacity) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (r->blocks[i] == r->blocks[j]) return 0;
    }
    return 1;
}
infs_status infs20_ref_encode(uint8_t *out, const struct infs20_ref *r,
                              uint64_t capacity)
{
    if (!out || !ref_valid(r, capacity)) return INFS_STATUS_INVALID_ARGUMENT;
    uint8_t bytes[INFS20_REF_BYTES];
    memcpy(bytes, r->id, 16);
    memcpy(bytes + 16, r->digest, 32);
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i)
        infs_store_le64(bytes + 48 + 8*i, r->blocks[i]);
    memcpy(out, bytes, sizeof(bytes));
    return INFS_STATUS_OK;
}
static infs_status ref_decode(const uint8_t *bytes, uint64_t capacity,
                               struct infs20_ref *r)
{
    memcpy(r->id, bytes, 16);
    memcpy(r->digest, bytes + 16, 32);
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i)
        r->blocks[i] = infs_load_le64(bytes + 48 + 8*i);
    return ref_valid(r, capacity) ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}

/* A tree payload is level:u16, count:u16, used:u32, then tightly packed
 * (key[16], value_len:u16, value). Branch keys are exact child minima;
 * branch values are direct child refs. Leaf values are bounded opaque bytes.
 * Empty leaves are legal roots only. No implicit padding or native structs. */
infs_status infs20_tree_encode(uint8_t *out, size_t out_size,
                              uint16_t level, const struct infs20_tree_entry *entries,
                              size_t count, size_t *used)
{
    if (!out || !used || out_size < INFS20_TREE_HEADER ||
        out_size > INFS20_PAYLOAD_BYTES || level > INFS20_MAX_LEVEL ||
        count > UINT16_MAX || (count && !entries) || (level && count < 2))
        return INFS_STATUS_INVALID_ARGUMENT;
    uint8_t buf[INFS20_PAYLOAD_BYTES] = {0};
    size_t pos = INFS20_TREE_HEADER;
    for (size_t i = 0; i < count; ++i) {
        const struct infs20_tree_entry *e = &entries[i];
        if ((i && memcmp(entries[i-1].key, e->key, 16) >= 0) ||
            (e->value_size && !e->value) ||
            (level && e->value_size != INFS20_REF_BYTES))
            return INFS_STATUS_INVALID_ARGUMENT;
        if (pos > out_size || out_size - pos < 18u ||
            e->value_size > out_size - pos - 18u)
            return INFS_STATUS_NO_SPACE;
        memcpy(buf + pos, e->key, 16);
        infs_store_le16(buf + pos + 16, e->value_size);
        if (e->value_size) memcpy(buf + pos + 18, e->value, e->value_size);
        pos += 18u + e->value_size;
    }
    infs_store_le16(buf, level);
    infs_store_le16(buf + 2, (uint16_t)count);
    infs_store_le32(buf + 4, (uint32_t)pos);
    memcpy(out, buf, pos);
    *used = pos;
    return INFS_STATUS_OK;
}
static int commit_valid(const struct infs20_commit *c, uint64_t seq)
{
    if (!c || !seq || c->previous_sequence != seq-1 ||
        (nonzero(c->previous_digest, 32) != (seq != 1)) ||
        !c->blocks || c->blocks > INFS20_MAX_MEMBER_BLOCKS) return 0;
    for (unsigned i = 0; i < INFS20_ROOTS; ++i) {
        if (!ref_valid(&c->roots[i], c->blocks)) return 0;
        for (unsigned j = 0; j < i; ++j) {
            if (!memcmp(c->roots[i].id, c->roots[j].id, 16)) return 0;
            for (unsigned a = 0; a < INFS20_LANE_COUNT; ++a)
                for (unsigned b = 0; b < INFS20_LANE_COUNT; ++b)
                    if (c->roots[i].blocks[a] == c->roots[j].blocks[b]) return 0;
        }
    }
    return 1;
}
infs_status infs20_commit_encode(uint8_t *out, size_t out_size,
                                const struct infs20_commit *c, uint64_t seq)
{
    if (!out || out_size < INFS20_COMMIT_BYTES || !commit_valid(c, seq))
        return INFS_STATUS_INVALID_ARGUMENT;
    uint8_t buf[INFS20_COMMIT_BYTES];
    infs_store_le64(buf, c->blocks);
    infs_store_le64(buf + 8, c->previous_sequence);
    memcpy(buf + 16, c->previous_digest, 32);
    for (unsigned i = 0; i < INFS20_ROOTS; ++i)
        if (infs20_ref_encode(buf + 48 + i*INFS20_REF_BYTES,
                              &c->roots[i], c->blocks) != INFS_STATUS_OK)
            return INFS_STATUS_INVALID_ARGUMENT;
    memcpy(out, buf, sizeof(buf));
    return INFS_STATUS_OK;
}
static infs_status commit_decode(const uint8_t *p, size_t size, uint64_t seq,
                                 uint64_t capacity, struct infs20_commit *c)
{
    if (size != INFS20_COMMIT_BYTES) return INFS_STATUS_CORRUPT;
    c->blocks = infs_load_le64(p);
    c->previous_sequence = infs_load_le64(p + 8);
    memcpy(c->previous_digest, p + 16, 32);
    /* Geometry changes deliberately not supported in this initial model. */
    if (c->blocks != capacity) return INFS_STATUS_NOT_SUPPORTED;
    for (unsigned i = 0; i < INFS20_ROOTS; ++i)
        if (ref_decode(p + 48 + i*INFS20_REF_BYTES, capacity, &c->roots[i]))
            return INFS_STATUS_CORRUPT;
    return commit_valid(c, seq) ? INFS_STATUS_OK : INFS_STATUS_CORRUPT;
}
static int io_valid(const struct infs20_io *io)
{
    if (!io || !io->read || !io->blocks || io->blocks > INFS20_MAX_MEMBER_BLOCKS ||
        !nonzero(io->set_id,16) || !nonzero(io->member_id,16)) return 0;
    for (unsigned i = 0; i < INFS20_COMMIT_SLOTS; ++i) {
        uint64_t block = io->slots[i/3][i%3];
        if (block >= io->blocks) return 0;
        for (unsigned j = 0; j < i; ++j)
            if (block == io->slots[j/3][j%3]) return 0;
    }
    return 1;
}
static int slot_block(const struct infs20_io *io, uint64_t block)
{
    for (unsigned i = 0; i < INFS20_COMMIT_SLOTS; ++i)
        if (block == io->slots[i/3][i%3]) return 1;
    return 0;
}
static infs_status read_ref(const struct infs20_io *io, const struct infs20_ref *r,
                            uint64_t seq, size_t *budget, uint8_t *page,
                            struct infs20_decoded_page *d)
{
    if (!ref_valid(r, io->blocks)) return INFS_STATUS_CORRUPT;
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i)
        if (slot_block(io, r->blocks[i])) return INFS_STATUS_CORRUPT;
    for (unsigned i = 0; i < INFS20_LANE_COUNT; ++i) {
        if (!*budget) return INFS_STATUS_BUSY;
        --*budget;
        if (io->read(io->context, r->blocks[i], page)) continue;
        if (infs20_page_decode(page, INFS20_PAGE_BYTES, io->member_id,
                               r->blocks[i], io->blocks, d)) continue;
        if (d->kind == INFS20_TREE_KIND && d->identity.lane == i &&
            d->identity.sequence <= seq && !memcmp(d->identity.set_id, io->set_id,16) &&
            !memcmp(d->identity.scope_id, io->set_id,16) &&
            !memcmp(d->identity.page_id,r->id,16) && !memcmp(d->digest,r->digest,32))
            return INFS_STATUS_OK;
    }
    return INFS_STATUS_CORRUPT;
}
/* Strictly decreasing levels bound recursion and reject cycles. Child minima
 * and upper bounds prevent a stale/shared node from satisfying incompatible
 * ranges. The explicit page-read budget bounds even a malicious wide tree.
 * Userspace stack only; this is not a kernel traversal implementation. */
static infs_status verify_node(const struct infs20_io *io, const struct infs20_ref *r,
                               uint64_t seq, int expected_level,
                               const uint8_t *lower, const uint8_t *upper,
                               size_t *budget)
{
    uint8_t page[INFS20_PAGE_BYTES];
    struct infs20_decoded_page d;
    infs_status st = read_ref(io,r,seq,budget,page,&d);
    if (st) return st;
    const uint8_t *p = page + INFS20_OFF_PAYLOAD;
    size_t len = d.payload_size;
    if (len < INFS20_TREE_HEADER) return INFS_STATUS_CORRUPT;
    unsigned level = infs_load_le16(p), count = infs_load_le16(p+2);
    if (level > INFS20_MAX_LEVEL ||
        (expected_level >= 0 && level != (unsigned)expected_level) ||
        infs_load_le32(p+4) != len || (level && count < 2) ||
        (!count && lower)) return INFS_STATUS_CORRUPT;
    size_t pos = INFS20_TREE_HEADER;
    const uint8_t *last = NULL;
    /* First pass validates all framing, so next-key access is safe. */
    for (unsigned i = 0; i < count; ++i) {
        if (len-pos < 18u) return INFS_STATUS_CORRUPT;
        const uint8_t *key = p+pos;
        size_t n = infs_load_le16(key+16);
        if (n > len-pos-18u || (level && n != INFS20_REF_BYTES) ||
            (last && memcmp(last,key,16)>=0) ||
            (!i && lower && memcmp(lower,key,16)!=0) ||
            (upper && memcmp(key,upper,16)>=0)) return INFS_STATUS_CORRUPT;
        last = key;
        pos += 18u+n;
    }
    if (pos != len) return INFS_STATUS_CORRUPT;
    if (!level) return INFS_STATUS_OK;
    pos = INFS20_TREE_HEADER;
    for (unsigned i = 0; i < count; ++i) {
        struct infs20_ref child;
        if (ref_decode(p+pos+18, io->blocks, &child)) return INFS_STATUS_CORRUPT;
        size_t next = pos+18+INFS20_REF_BYTES;
        st = verify_node(io,&child,seq,(int)level-1,p+pos,
                         i+1<count ? p+next : upper,budget);
        if (st) return st;
        pos = next;
    }
    return INFS_STATUS_OK;
}
infs_status infs20_verify_tree(const struct infs20_io *io,
                               const struct infs20_ref *root,
                               uint64_t sequence, size_t *budget)
{
    if (!io_valid(io) || !root || !sequence || !budget)
        return INFS_STATUS_INVALID_ARGUMENT;
    return verify_node(io,root,sequence,-1,NULL,NULL,budget);
}
static infs_status verify_roots(const struct infs20_io *io,
                                const struct infs20_commit *c, uint64_t seq,
                                size_t budget)
{
    for (unsigned i=0; i<INFS20_ROOTS; ++i) {
        infs_status st = infs20_verify_tree(io,&c->roots[i],seq,&budget);
        if (st) return st;
    }
    return INFS_STATUS_OK;
}
infs_status infs20_select(const struct infs20_io *io, size_t budget,
                          struct infs20_authority *out)
{
    if (!io_valid(io) || !out) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_authority best = {0};
    struct infs20_authority seen[INFS20_COMMIT_SLOTS];
    unsigned nseen=0;
    for (unsigned bank=0; bank<2; ++bank) for (unsigned lane=0; lane<3; ++lane) {
        uint8_t page[INFS20_PAGE_BYTES];
        struct infs20_decoded_page d;
        uint64_t block=io->slots[bank][lane];
        infs_status st=io->read(io->context,block,page);
        /* An unreadable slot may hide a newer authority. Refuse writable
         * selection instead of silently treating I/O error as a blank slot. */
        if (st) return st;
        st=infs20_page_decode(page,sizeof(page),io->member_id,block,io->blocks,&d);
        if (st) {
            /* A valid-CRC but unknown experimental version must not be
             * downgraded. Unrecognised magic/torn bytes are not evidence. */
            if (st==INFS_STATUS_NOT_SUPPORTED && !memcmp(page,"INF20S0!",8))
                return st;
            continue;
        }
        if (d.kind!=INFS20_COMMIT_KIND || d.identity.lane!=lane ||
            memcmp(d.identity.set_id,io->set_id,16) ||
            memcmp(d.identity.scope_id,io->set_id,16))
            return INFS_STATUS_CORRUPT;
        struct infs20_authority a={0};
        a.sequence=d.identity.sequence; a.bank=(uint8_t)bank;
        a.lane_mask=(uint8_t)(1u<<lane);
        memcpy(a.digest,d.digest,32); memcpy(a.page_id,d.identity.page_id,16);
        st=commit_decode(page+INFS20_OFF_PAYLOAD,d.payload_size,a.sequence,
                         io->blocks,&a.commit);
        if (st) return st;
        for (unsigned i=0; i<nseen; ++i)
            if (seen[i].sequence==a.sequence &&
                (seen[i].bank!=a.bank || memcmp(seen[i].digest,a.digest,32)))
                return INFS_STATUS_CORRUPT;
        seen[nseen++]=a;
        if (a.sequence>best.sequence) best=a;
        else if (a.sequence==best.sequence) best.lane_mask|=a.lane_mask;
    }
    if (!best.sequence) return INFS_STATUS_NOT_FOUND;
    for (unsigned i=0; i<nseen; ++i)
        if (seen[i].sequence==best.commit.previous_sequence &&
            memcmp(seen[i].digest,best.commit.previous_digest,32))
            return INFS_STATUS_CORRUPT;
    infs_status st=verify_roots(io,&best.commit,best.sequence,budget);
    if (st) return st;
    *out=best;
    return INFS_STATUS_OK;
}
infs_status infs20_publish(const struct infs20_io *io,
                           const struct infs20_authority *previous,
                           const struct infs20_commit *next,
                           const uint8_t page_id[16], size_t budget,
                           struct infs20_authority *out)
{
    if (!io_valid(io) || !io->write || !io->flush || !next || !page_id ||
        !nonzero(page_id,16) || !out || next->blocks!=io->blocks)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_authority current;
    infs_status st=infs20_select(io,budget,&current);
    if (previous) {
        if (st) return st;
        if (previous->sequence!=current.sequence ||
            memcmp(previous->digest,current.digest,32) ||
            current.lane_mask!=7u) return INFS_STATUS_BUSY;
        if (current.sequence==UINT64_MAX) return INFS_STATUS_OVERFLOW;
        if (next->previous_sequence!=current.sequence ||
            memcmp(next->previous_digest,current.digest,32) ||
            !memcmp(page_id,current.page_id,16)) return INFS_STATUS_INVALID_ARGUMENT;
    } else if (st!=INFS_STATUS_NOT_FOUND) {
        return st ? st : INFS_STATUS_BUSY;
    }
    uint64_t seq=previous?current.sequence+1:1;
    unsigned bank=previous?1u-current.bank:0;
    uint8_t payload[INFS20_COMMIT_BYTES];
    st=infs20_commit_encode(payload,sizeof(payload),next,seq);
    if (st) return st;
    /* Establish dependency durability before writing any authority record.
     * Caller owns fresh allocation and prior-root pinning; this function
     * cannot establish those invariants without the future allocator. */
    st=io->flush(io->context);
    if (st) return st;
    st=verify_roots(io,next,seq,budget);
    if (st) return st;
    struct infs20_authority published={0};
    published.commit=*next; published.sequence=seq; published.bank=(uint8_t)bank;
    memcpy(published.page_id,page_id,16);
    for (unsigned lane=0; lane<3; ++lane) {
        struct infs20_page_identity id={0};
        memcpy(id.set_id,io->set_id,16); memcpy(id.scope_id,io->set_id,16);
        memcpy(id.page_id,page_id,16); memcpy(id.member_id,io->member_id,16);
        id.sequence=seq; id.block=io->slots[bank][lane]; id.lane=(uint16_t)lane;
        uint8_t page[INFS20_PAGE_BYTES];
        st=infs20_page_encode_kind(page,sizeof(page),&id,INFS20_COMMIT_KIND,
                                   payload,sizeof(payload));
        if (st) return st;
        st=io->write(io->context,id.block,page);
        if (st) return st;
        st=io->flush(io->context);
        if (st) return st;
        memcpy(published.digest,page+INFS20_OFF_DIGEST,32);
        published.lane_mask|=(uint8_t)(1u<<lane);
    }
    *out=published;
    return INFS_STATUS_OK;
}
