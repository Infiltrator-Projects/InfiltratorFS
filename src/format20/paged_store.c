// SPDX-License-Identifier: GPL-3.0-or-later
#include "paged_store.h"
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include <stdlib.h>
#include <string.h>

/*
 * P20A is the sole descriptor inside the existing commit tree. It commits
 * exact arena geometry/frontiers and the root reference. It is not a
 * second namespace: all keys and values live in the paged tree.
 * Appending and consuming committed free runs never overwrite either
 * selectable root. Explicit collection republishes the unchanged tree
 * root into the alternate bank before its unreachable pages become free.
 * Fresh transaction IDs distinguish aborted private page incarnations.
 */
enum { ROOT_OFFSET=80, RECLAIM_OFFSET=160, DESCRIPTOR_BYTES=272,
       PAGES_PER_CHANGE=3*(2*(INFS20_TREE_MAX_LEVEL+1)+1) };
static const uint8_t descriptor_key[]={'P','2','0','A'};
struct staged {
    uint64_t block;
    uint8_t page[INFS20_PAGE_BYTES];
};
struct mutation {
    const struct infs20_paged_layout *layout;
    const struct infs20_store_io *storage;
    struct infs20_paged_state state;
    uint8_t tx[16];
    uint64_t serial;
    struct staged *pages;
    size_t count,capacity,limit;
    uint64_t *reclaim_slots;
    size_t reclaim_count,reclaim_index;
    infs20_store_prepare payload_prepare;
    void *payload_context;
};
struct reader {
    const struct infs20_paged_layout *layout;
    const struct infs20_store_io *storage;
    const struct infs20_paged_state *state;
};
static int zero(const uint8_t *p,size_t n)
{
    uint8_t v=0; while(n--) v|=*p++; return v==0;
}
static int intersects(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{
    return n && m && a<b+m && b<a+n;
}
static int valid_layout(const struct infs20_paged_layout *l,
                        const struct infs20_store_io *io,int write)
{
    if(!l || !io || !io->read || (write && (!io->write || !io->flush)) ||
       !l->bootstrap.member_blocks ||
       l->bootstrap.member_blocks>INFS20_MAX_MEMBER_BLOCKS ||
       l->bootstrap.member_blocks<INFS20_STORE_LANE_BLOCKS ||
       zero(l->bootstrap.set_id,16) || zero(l->bootstrap.scope_id,16) ||
       zero(l->bootstrap.member_id,16)) return 0;
    uint64_t size=l->bootstrap.member_blocks;
    for(unsigned i=0;i<3;++i) {
        if(l->bootstrap.lane_start[i]>size-INFS20_STORE_LANE_BLOCKS ||
           !l->arena_blocks[i] || l->arena_start[i]>=size ||
           l->arena_blocks[i]>size-l->arena_start[i]) return 0;
    }
    for(unsigned i=0;i<3;++i) {
        for(unsigned j=0;j<3;++j)
            if(intersects(l->arena_start[i],l->arena_blocks[i],
                          l->bootstrap.lane_start[j],INFS20_STORE_LANE_BLOCKS))
                return 0;
        for(unsigned j=0;j<i;++j)
            if(intersects(l->arena_start[i],l->arena_blocks[i],
                          l->arena_start[j],l->arena_blocks[j]) ||
               intersects(l->bootstrap.lane_start[i],INFS20_STORE_LANE_BLOCKS,
                          l->bootstrap.lane_start[j],INFS20_STORE_LANE_BLOCKS))
                return 0;
    }
    return 1;
}
static int empty_root(const struct infs20_tree_ref *r)
{
    return zero(r->id,16) && zero(r->digest,32) && !r->sequence &&
           !r->blocks[0] && !r->blocks[1] && !r->blocks[2];
}
/* Geometry validation is independent of allocator reachability. Ordinary
 * reads consume authenticated references, while writers prove the frozen
 * reachability root and current-root disjointness before reservation. */
static int ref_valid(const struct infs20_paged_layout *l,
    const struct infs20_tree_ref *r,uint64_t limit,uint64_t sequence)
{
    if(empty_root(r)) return 1;
    if(zero(r->id,16) || zero(r->digest,32) || !r->sequence || r->sequence>sequence)
        return 0;
    for(unsigned lane=0;lane<3;++lane)
        if(r->blocks[lane]<l->arena_start[lane] ||
           r->blocks[lane]-l->arena_start[lane]>=limit ||
           r->blocks[lane]-l->arena_start[lane]!=r->blocks[0]-l->arena_start[0])
            return 0;
    return 1;
}
static int root_valid(const struct infs20_paged_layout *l,
                      const struct infs20_paged_state *s)
{
    if(!ref_valid(l,&s->root,s->used[0],s->commit.sequence)) return 0;
    if(!s->reclaim_epoch)
        return empty_root(&s->reclaim_root) && !s->reclaim_limit &&
               !s->reclaim_cursor && !s->free_slots;
    return s->reclaim_epoch<=s->commit.sequence &&
           s->reclaim_cursor<=s->reclaim_limit && s->reclaim_limit<=s->used[0] &&
           s->free_slots<=s->reclaim_limit-s->reclaim_cursor &&
           ref_valid(l,&s->reclaim_root,s->reclaim_limit,s->reclaim_epoch);
}
static void encode_ref(uint8_t *p,const struct infs20_tree_ref *r)
{
    memcpy(p,r->id,16); memcpy(p+16,r->digest,32);
    infs_store_le64(p+48,r->sequence);
    for(unsigned i=0;i<3;++i) infs_store_le64(p+56+8*i,r->blocks[i]);
}
static void decode_ref(const uint8_t *p,struct infs20_tree_ref *r)
{
    memcpy(r->id,p,16); memcpy(r->digest,p+16,32);
    r->sequence=infs_load_le64(p+48);
    for(unsigned i=0;i<3;++i) r->blocks[i]=infs_load_le64(p+56+8*i);
}
/* The fixed wire grammar is encoded fieldwise. No host struct padding. */
static void encode(const struct infs20_paged_layout *l,
    const struct infs20_paged_state *s,uint8_t p[DESCRIPTOR_BYTES])
{
    memset(p,0,DESCRIPTOR_BYTES);
    memcpy(p,"P20A",4); infs_store_le16(p+4,3);
    for(unsigned i=0;i<3;++i) {
        infs_store_le64(p+8+8*i,l->arena_start[i]);
        infs_store_le64(p+32+8*i,l->arena_blocks[i]);
        infs_store_le64(p+56+8*i,s->used[i]);
    }
    encode_ref(p+ROOT_OFFSET,&s->root);
    encode_ref(p+RECLAIM_OFFSET,&s->reclaim_root);
    infs_store_le64(p+240,s->reclaim_epoch);
    infs_store_le64(p+248,s->reclaim_limit);
    infs_store_le64(p+256,s->reclaim_cursor);
    infs_store_le64(p+264,s->free_slots);
}
static infs_status decode(const struct infs20_paged_layout *l,
    const uint8_t p[DESCRIPTOR_BYTES],struct infs20_paged_state *s)
{
    if(memcmp(p,"P20A",4) || infs_load_le16(p+4)!=3)
        return INFS_STATUS_NOT_SUPPORTED;
    if(!zero(p+6,2)) return INFS_STATUS_CORRUPT;
    for(unsigned i=0;i<3;++i) {
        if(infs_load_le64(p+8+8*i)!=l->arena_start[i] ||
           infs_load_le64(p+32+8*i)!=l->arena_blocks[i])
            return INFS_STATUS_CORRUPT;
        s->used[i]=infs_load_le64(p+56+8*i);
        if(s->used[i]>l->arena_blocks[i] || (i && s->used[i]!=s->used[0]))
            return INFS_STATUS_CORRUPT;
    }
    decode_ref(p+ROOT_OFFSET,&s->root);
    decode_ref(p+RECLAIM_OFFSET,&s->reclaim_root);
    s->reclaim_epoch=infs_load_le64(p+240);
    s->reclaim_limit=infs_load_le64(p+248);
    s->reclaim_cursor=infs_load_le64(p+256);
    s->free_slots=infs_load_le64(p+264);
    return root_valid(l,s)?INFS_STATUS_OK:INFS_STATUS_CORRUPT;
}
static infs_status descriptor(const struct infs20_store_layout *b,
    const struct infs20_store_io *io,uint8_t p[DESCRIPTOR_BYTES],
    struct infs20_store_state *s)
{
    infs_status st=infs20_store_recover(b,io,s);
    if(st) return st;
    /* An existing different model is not an empty paged store. */
    if(s->records!=1) return INFS_STATUS_NOT_SUPPORTED;
    size_t size=0; uint8_t raw[INFS20_PAGE_BYTES];
    st=infs20_store_lookup(b,io,descriptor_key,sizeof(descriptor_key),
                           raw,sizeof(raw),&size);
    if(st==INFS_STATUS_NOT_FOUND) return INFS_STATUS_NOT_SUPPORTED;
    if(st) return st;
    if(size>=6 && !memcmp(raw,"P20A",4) && infs_load_le16(raw+4)!=3)
        return INFS_STATUS_NOT_SUPPORTED;
    if(size!=DESCRIPTOR_BYTES) return INFS_STATUS_CORRUPT;
    memcpy(p,raw,size); return INFS_STATUS_OK;
}
static int permitted(const struct infs20_paged_layout *l,
    const struct infs20_paged_state *s,uint64_t block)
{
    for(unsigned lane=0;lane<3;++lane)
        if(block>=l->arena_start[lane] &&
           block-l->arena_start[lane]<s->used[lane]) return 1;
    return 0;
}
static infs_status read_tree(void *o,uint64_t b,uint8_t *p)
{
    struct reader *r=o;
    /* Do not follow a locator outside the committed arena frontiers.
     * Tree identity/digest/ordinal validation supplies the other half
     * of the ownership check. */
    if(!permitted(r->layout,r->state,b)) return INFS_STATUS_CORRUPT;
    return r->storage->read(r->storage->context,b,p);
}
static struct infs20_tree_io tree_reader(struct reader *r)
{
    struct infs20_tree_io io={0}; io.context=r;
    memcpy(io.set_id,r->layout->bootstrap.set_id,16);
    memcpy(io.scope_id,r->layout->bootstrap.scope_id,16);
    memcpy(io.member_id,r->layout->bootstrap.member_id,16);
    io.member_blocks=r->layout->bootstrap.member_blocks;
    io.sequence=r->state->commit.sequence; io.read=read_tree;
    return io;
}
/* One replica-admission contract for mutation, publication and arena GC.
 * Read-only traversal may combine surviving copies, but a protected writer
 * must verify all declared lane addresses, scope, identity and digest. */
static infs_status verify_ref_copies(const struct infs20_paged_layout *l,
    const struct infs20_paged_state *s,const struct infs20_store_io *io,
    const struct infs20_tree_ref *ref,uint64_t *slot_out)
{
    uint64_t slot=0;
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
        if(ref->blocks[lane]<l->arena_start[lane]) return INFS_STATUS_CORRUPT;
        uint64_t n=ref->blocks[lane]-l->arena_start[lane];
        if(n>=s->used[lane] || (lane && n!=slot))
            return INFS_STATUS_CORRUPT;
        slot=n;
        uint8_t page[INFS20_PAGE_BYTES];
        struct infs20_decoded_page d;
        infs_status st=io->read(io->context,ref->blocks[lane],page);
        if(st) return st;
        const struct infs20_store_layout *b=&l->bootstrap;
        st=infs20_page_decode(page,sizeof(page),b->member_id,ref->blocks[lane],
                               b->member_blocks,&d);
        if(st || d.identity.lane!=lane || d.identity.sequence!=ref->sequence ||
           memcmp(d.identity.set_id,b->set_id,16) ||
           memcmp(d.identity.scope_id,b->scope_id,16) ||
           memcmp(d.identity.page_id,ref->id,16) || memcmp(d.digest,ref->digest,32))
            return INFS_STATUS_BUSY;
    }
    if(slot_out) *slot_out=slot;
    return INFS_STATUS_OK;
}
struct marks {
    const struct infs20_paged_layout *layout;
    const struct infs20_paged_state *state;
    const struct infs20_store_io *storage;
    uint64_t *slots;
    size_t count,capacity;
};
static infs_status mark_ref(void *opaque,const struct infs20_tree_ref *r)
{
    struct marks *m=opaque;
    uint64_t slot=0;
    infs_status st=verify_ref_copies(m->layout,m->state,m->storage,r,&slot);
    if(st) return st;
    if((uint64_t)m->count>=m->state->used[0])
        return INFS_STATUS_CORRUPT;
    if(m->count==m->capacity) {
        if(m->capacity>SIZE_MAX/2/sizeof(*m->slots)) return INFS_STATUS_OVERFLOW;
        size_t cap=m->capacity?m->capacity*2:16;
        uint64_t *next=realloc(m->slots,cap*sizeof(*next));
        if(!next) return INFS_STATUS_NO_MEMORY;
        m->slots=next; m->capacity=cap;
    }
    m->slots[m->count++]=slot;
    return INFS_STATUS_OK;
}
static int slot_cmp(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;
    return (x>y)-(x<y);
}
/* Trace exactly the authenticated root; duplicate relative slots or
 * references outside the frozen frontier cannot authorize reclamation. */
static infs_status trace_root(struct reader *r,const struct infs20_tree_ref *root,
    uint64_t limit,struct marks *m)
{
    struct infs20_tree_io tree=tree_reader(r);
    infs_status st=infs20_tree_visit_refs(&tree,root,mark_ref,m);
    if(st) return st;
    if(m->count>1) qsort(m->slots,m->count,sizeof(*m->slots),slot_cmp);
    for(size_t i=0;i<m->count;++i)
        if(m->slots[i]>=limit || (i && m->slots[i-1]>=m->slots[i]))
            return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}
static size_t lower_slot(const uint64_t *slots,size_t count,uint64_t target)
{
    size_t first=0,last=count;
    while(first<last) {
        size_t mid=first+(last-first)/2;
        if(slots[mid]<target) first=mid+1; else last=mid;
    }
    return first;
}
static int available_slot(const struct infs20_paged_state *s,const struct marks *m,
    uint64_t slot)
{
    if(!s->reclaim_epoch || slot<s->reclaim_cursor || slot>=s->reclaim_limit) return 0;
    size_t i=lower_slot(m->slots,m->count,slot);
    return i==m->count || m->slots[i]!=slot;
}
static infs_status trace_reclaim(struct reader *r,struct marks *m)
{
    const struct infs20_paged_state *s=r->state;
    if(!s->reclaim_epoch) return INFS_STATUS_OK;
    infs_status st=trace_root(r,&s->reclaim_root,s->reclaim_limit,m);
    if(st) return st;
    size_t first=lower_slot(m->slots,m->count,s->reclaim_cursor);
    uint64_t occupied=(uint64_t)(m->count-first);
    uint64_t remaining=s->reclaim_limit-s->reclaim_cursor;
    if(occupied>remaining || remaining-occupied!=s->free_slots)
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}
static infs_status verify_current(struct reader *r,const struct marks *reclaim)
{
    struct marks current={r->layout,r->state,r->storage,NULL,0,0};
    infs_status st=trace_root(r,&r->state->root,r->state->used[0],&current);
    if(!st) for(size_t i=0;i<current.count;++i)
        if(available_slot(r->state,reclaim,current.slots[i])) {
            st=INFS_STATUS_CORRUPT; break;
        }
    free(current.slots); return st;
}
static infs_status verify_protection(void *opaque)
{
    struct reader *r=opaque;
    struct marks reclaim={r->layout,r->state,r->storage,NULL,0,0};
    infs_status st=trace_reclaim(r,&reclaim);
    if(!st) st=verify_current(r,&reclaim);
    free(reclaim.slots); return st;
}
infs_status infs20_paged_open(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,struct infs20_paged_state *out)
{
    if(!out || !valid_layout(l,io,0)) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_paged_state s={0};
    uint8_t p[DESCRIPTOR_BYTES];
    infs_status st=descriptor(&l->bootstrap,io,p,&s.commit);
    if(!st) st=decode(l,p,&s);
    /* Open validates the constant-size authority descriptor only.
     * Consumed tree pages are independently validated by get/visit.
     * Full offline proof is explicit, not a hidden mount scan. */
    if(!st) *out=s;
    return st;
}
struct publication_verification {
    struct reader reader;
    infs20_store_prepare payload_verify;
    void *payload_context;
};
/* Both metadata and newly staged data must read back correctly after the
 * dependency barrier; a successful write alone is not publication authority. */
static infs_status verify_publication(void *opaque)
{
    struct publication_verification *v=opaque;
    infs_status st=verify_protection(&v->reader);
    if(!st && v->payload_verify) st=v->payload_verify(v->payload_context);
    return st;
}
static infs_status publish(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    struct infs20_paged_state *s,infs20_store_prepare prepare,void *opaque,
    infs20_store_prepare payload_verify,void *payload_context)
{
    uint8_t p[DESCRIPTOR_BYTES]; encode(l,s,p);
    struct infs20_store_record record={descriptor_key,p,sizeof(descriptor_key),
                                       DESCRIPTOR_BYTES};
    struct infs20_store_state committed;
    /* Verify the candidate's real stored pages after dependency flush. The
     * private root may contain pages born in the upcoming generation. */
    struct infs20_paged_state candidate=*s;
    ++candidate.commit.sequence;
    struct publication_verification verification={{l,io,&candidate},payload_verify,payload_context};
    infs_status st=infs20_store_publish_verified(&l->bootstrap,io,tx,&record,1,
        prepare,opaque,verify_publication,&verification,&committed);
    if(!st) s->commit=committed;
    return st;
}
static infs_status blank_bootstrap(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io)
{
    struct infs20_store_state old;
    infs_status st=infs20_store_recover(&l->bootstrap,io,&old);
    if(!st) return INFS_STATUS_ALREADY_EXISTS;
    if(st!=INFS_STATUS_NOT_FOUND) return st;
    uint8_t page[INFS20_PAGE_BYTES];
    for(unsigned lane=0;lane<3;++lane)
        for(unsigned i=0;i<INFS20_STORE_LANE_BLOCKS;++i) {
            st=io->read(io->context,l->bootstrap.lane_start[lane]+i,page);
            if(st) return st;
            if(!zero(page,sizeof(page))) return INFS_STATUS_CORRUPT;
        }
    return INFS_STATUS_OK;
}
infs_status infs20_paged_create(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    struct infs20_paged_state *out)
{
    return infs20_paged_apply(l,io,tx,NULL,0,1,NULL,NULL,NULL,out);
}
static infs_status mutation_read(void *o,uint64_t b,uint8_t *p)
{
    struct mutation *m=o;
    for(size_t i=0;i<m->count;++i) if(m->pages[i].block==b) {
        memcpy(p,m->pages[i].page,INFS20_PAGE_BYTES);
        return INFS_STATUS_OK;
    }
    if(!permitted(m->layout,&m->state,b)) return INFS_STATUS_CORRUPT;
    return m->storage->read(m->storage->context,b,p);
}
static infs_status reserve(void *o,uint8_t id[16],uint64_t blocks[3])
{
    struct mutation *m=o;
    int append=1;
    for(unsigned i=0;i<3;++i)
        if(m->state.used[i]==m->layout->arena_blocks[i]) append=0;
    if(!append && !m->state.free_slots) return INFS_STATUS_NO_SPACE;
    /* IDs are transaction-scoped and never reused after uncertain write.
     * Fresh tx is a caller contract; the sequence additionally separates
     * successful generations. A private serial separates split pages. */
    uint8_t preimage[36]={'P','2','0','I'},digest[32];
    memcpy(preimage+4,m->tx,16);
    infs_store_le64(preimage+20,m->state.commit.sequence+1);
    infs_store_le64(preimage+28,++m->serial);
    infs_sha256(preimage,sizeof(preimage),digest);
    memcpy(id,digest,16);
    if(append) {
        for(unsigned i=0;i<3;++i)
            blocks[i]=m->layout->arena_start[i]+m->state.used[i]++;
    } else {
        /* The frozen root remains protected even if current-root edits
         * make some of its pages obsolete. Never recycle a mark or a slot
         * below the committed cursor until a new collection republishes. */
        uint64_t slot=m->state.reclaim_cursor;
        while(m->reclaim_index<m->reclaim_count && m->reclaim_slots[m->reclaim_index]<slot)
            ++m->reclaim_index;
        while(m->reclaim_index<m->reclaim_count && m->reclaim_slots[m->reclaim_index]==slot) {
            ++slot; ++m->reclaim_index;
        }
        if(slot>=m->state.reclaim_limit) return INFS_STATUS_CORRUPT;
        m->state.reclaim_cursor=slot+1; --m->state.free_slots;
        for(unsigned i=0;i<3;++i) blocks[i]=m->layout->arena_start[i]+slot;
    }
    return INFS_STATUS_OK;
}
static infs_status stage(void *o,uint64_t b,const uint8_t *p)
{
    struct mutation *m=o;
    if(!permitted(m->layout,&m->state,b)) return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<m->count;++i)
        if(m->pages[i].block==b) return INFS_STATUS_CORRUPT;
    if(m->count==m->limit) return INFS_STATUS_NO_SPACE;
    if(m->count==m->capacity) {
        /* Limit both growth and byte multiplication before realloc. */
        size_t cap=m->capacity ?
            (m->capacity>m->limit/2 ? m->limit : m->capacity*2) : 6;
        if(cap>m->limit) cap=m->limit;
        struct staged *next=realloc(m->pages,cap*sizeof(*next));
        if(!next) return INFS_STATUS_NO_MEMORY;
        m->pages=next; m->capacity=cap;
    }
    m->pages[m->count].block=b;
    memcpy(m->pages[m->count++].page,p,INFS20_PAGE_BYTES);
    return INFS_STATUS_OK;
}
static infs_status prepare(void *opaque)
{
    struct mutation *m=opaque;
    if(m->payload_prepare) {
        infs_status st=m->payload_prepare(m->payload_context);
        if(st) return st;
    }
    for(size_t i=0;i<m->count;++i) {
        infs_status st=m->storage->write(m->storage->context,m->pages[i].block,
                                         m->pages[i].page);
        if(st) return st;
    }
    /* Existing store flushes all dependencies before the first commit. */
    return INFS_STATUS_OK;
}
infs_status infs20_paged_apply(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    const struct infs20_paged_change *changes,size_t count,int create_empty,
    infs20_store_prepare payload_prepare,infs20_store_prepare payload_verify,void *payload_context,
    struct infs20_paged_state *out)
{
    if(!out || !tx || zero(tx,16) || !valid_layout(l,io,1) ||
       (count && !changes) ||
       (create_empty!=0 && create_empty!=1))
        return INFS_STATUS_INVALID_ARGUMENT;
    if(count>SIZE_MAX/sizeof(*changes) ||
       count>SIZE_MAX/(size_t)PAGES_PER_CHANGE)
        return INFS_STATUS_OVERFLOW;
    /* Validate the public request before touching even private staging.
     * Changes are ordered: a later operation may see a key inserted by
     * an earlier operation in the same transaction. */
    for(size_t i=0;i<count;++i) {
        const struct infs20_paged_change *x=changes+i;
        if(!x->key || !x->key_size || x->key_size>INFS20_TREE_KEY_MAX ||
           (x->remove!=0 && x->remove!=1) ||
           (x->remove && x->value_size) || (x->value_size && !x->value))
            return INFS_STATUS_INVALID_ARGUMENT;
    }
    struct mutation m={0}; m.layout=l; m.storage=io;
    m.limit=count*(size_t)PAGES_PER_CHANGE;
    if(m.limit>SIZE_MAX/sizeof(*m.pages))
        m.limit=SIZE_MAX/sizeof(*m.pages);
    m.payload_prepare=payload_prepare; m.payload_context=payload_context;
    infs_status st;
    if(create_empty) st=blank_bootstrap(l,io);
    else {
        st=infs20_paged_open(l,io,&m.state);
        if(!st && (!m.state.commit.fully_protected ||
                   !memcmp(tx,m.state.commit.transaction_id,16)))
            st=INFS_STATUS_BUSY;
        /* Bounded prototype: full admission trace before any private
         * reservation or payload callback. Production should carry verified
         * protection state and check affected dependencies incrementally. */
        if(!st) {
            struct reader current={l,io,&m.state};
            struct marks reclaim={l,&m.state,io,NULL,0,0};
            st=trace_reclaim(&current,&reclaim);
            if(!st) st=verify_current(&current,&reclaim);
            m.reclaim_slots=reclaim.slots; m.reclaim_count=reclaim.count;
        }
    }
    if(st) { free(m.reclaim_slots); return st; }
    if(m.state.commit.sequence==UINT64_MAX) { free(m.reclaim_slots); return INFS_STATUS_OVERFLOW; }
    memcpy(m.tx,tx,16);
    struct reader r={l,io,&m.state};
    struct infs20_tree_io tree=tree_reader(&r);
    tree.context=&m; tree.read=mutation_read; tree.write=stage; tree.reserve=reserve;
    ++tree.sequence;
    for(size_t i=0;i<count;++i) {
        const struct infs20_paged_change *x=changes+i;
        struct infs20_tree_ref root;
        st=x->remove?infs20_tree_remove(&tree,&m.state.root,x->key,x->key_size,&root):
           infs20_tree_put(&tree,&m.state.root,x->key,x->key_size,x->value,x->value_size,&root);
        if(st) break;
        m.state.root=root;
    }
    if(!st) st=publish(l,io,tx,&m.state,prepare,&m,payload_verify,payload_context);
    if(!st) *out=m.state;
    free(m.reclaim_slots); free(m.pages);
    return st;
}
infs_status infs20_paged_put(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const uint8_t *key,
    size_t ks,const uint8_t *value,size_t vs,struct infs20_paged_state *out)
{
    const struct infs20_paged_change change={key,value,ks,vs,0};
    return infs20_paged_apply(l,io,tx,&change,1,0,NULL,NULL,NULL,out);
}
infs_status infs20_paged_remove(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const uint8_t *key,
    size_t ks,struct infs20_paged_state *out)
{
    const struct infs20_paged_change change={key,NULL,ks,0,1};
    return infs20_paged_apply(l,io,tx,&change,1,0,NULL,NULL,NULL,out);
}
struct replacement {
    const struct infs20_store_record *next;
    size_t count,index,changes,capacity;
    struct infs20_paged_change *batch;
    uint8_t **owned;
};
static int record_keycmp(const uint8_t *a,size_t an,const uint8_t *b,size_t bn)
{
    size_t n=an<bn?an:bn;
    int cmp=memcmp(a,b,n);
    return cmp?cmp:(an>bn)-(an<bn);
}
/* The diff is private until the one common publication. Grow only for
 * actual edits, not every unchanged record, and retain ownership on OOM. */
static infs_status grow_diff(struct replacement *d)
{
    if(d->changes<d->capacity) return INFS_STATUS_OK;
    size_t max=SIZE_MAX/sizeof(*d->batch);
    if(max>SIZE_MAX/sizeof(*d->owned)) max=SIZE_MAX/sizeof(*d->owned);
    if(d->capacity==max) return INFS_STATUS_OVERFLOW;
    size_t cap=d->capacity ?
        (d->capacity>max/2 ? max : d->capacity*2) : 16;
    if(cap>max) cap=max;
    struct infs20_paged_change *batch=realloc(d->batch,cap*sizeof(*batch));
    if(!batch) return INFS_STATUS_NO_MEMORY;
    d->batch=batch;
    uint8_t **owned=realloc(d->owned,cap*sizeof(*owned));
    if(!owned) return INFS_STATUS_NO_MEMORY;
    d->owned=owned; d->capacity=cap;
    return INFS_STATUS_OK;
}
static infs_status queue_put(struct replacement *d)
{
    infs_status st=grow_diff(d); if(st) return st;
    d->owned[d->changes]=NULL;
    const struct infs20_store_record *r=d->next+d->index++;
    d->batch[d->changes++]=(struct infs20_paged_change){
        r->key,r->value,r->key_size,r->value_size,0};
    return INFS_STATUS_OK;
}
static infs_status diff_visit(void *opaque,const uint8_t *key,size_t ks,
    const uint8_t *value,size_t vs)
{
    struct replacement *d=opaque;
    while(d->index<d->count) {
        const struct infs20_store_record *r=d->next+d->index;
        int cmp=record_keycmp(r->key,r->key_size,key,ks);
        if(cmp>0) break;
        if(!cmp) {
            if(r->value_size==vs && (!vs || !memcmp(r->value,value,vs))) {
                ++d->index;
                return INFS_STATUS_OK;
            }
            return queue_put(d);
        }
        infs_status st=queue_put(d); if(st) return st;
    }
    infs_status st=grow_diff(d); if(st) return st;
    /* The visitor's key is borrowed; removal must outlive this callback. */
    uint8_t *copy=malloc(ks);
    if(!copy) return INFS_STATUS_NO_MEMORY;
    memcpy(copy,key,ks);
    size_t i=d->changes++;
    d->owned[i]=copy;
    d->batch[i]=(struct infs20_paged_change){copy,NULL,ks,0,1};
    return INFS_STATUS_OK;
}
infs_status infs20_paged_replace(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    const struct infs20_store_record *records,size_t count,int create_empty,
    infs20_store_prepare prepare_payload,infs20_store_prepare verify_payload,
    void *context,struct infs20_paged_state *out)
{
    if(!out || !tx || zero(tx,16) || !valid_layout(l,io,1) ||
       (count && !records) ||
       (create_empty!=0 && create_empty!=1)) return INFS_STATUS_INVALID_ARGUMENT;
    if(count>SIZE_MAX/sizeof(*records)) return INFS_STATUS_OVERFLOW;
    for(size_t i=0;i<count;++i) {
        const struct infs20_store_record *r=records+i;
        if(!r->key || !r->key_size || r->key_size>INFS20_TREE_KEY_MAX ||
           (r->value_size && !r->value) ||
           (i && record_keycmp(records[i-1].key,records[i-1].key_size,
                               r->key,r->key_size)>=0))
            return INFS_STATUS_INVALID_ARGUMENT;
    }
    struct replacement *d=calloc(1,sizeof(*d));
    if(!d) return INFS_STATUS_NO_MEMORY;
    d->next=records; d->count=count;
    infs_status st=create_empty?INFS_STATUS_OK:infs20_paged_visit(l,io,diff_visit,d);
    while(!st && d->index<count) st=queue_put(d);
    if(!st) st=infs20_paged_apply(l,io,tx,d->batch,d->changes,create_empty,
                                  prepare_payload,verify_payload,context,out);
    for(size_t i=0;i<d->changes;++i) free(d->owned[i]);
    free(d->owned); free(d->batch); free(d);
    return st;
}
infs_status infs20_paged_get(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t *key,size_t ks,
    uint8_t *out,size_t capacity,size_t *size)
{
    struct infs20_paged_state s;
    infs_status st=infs20_paged_open(l,io,&s); if(st) return st;
    struct reader r={l,io,&s}; struct infs20_tree_io tree=tree_reader(&r);
    return infs20_tree_get(&tree,&s.root,key,ks,out,capacity,size);
}
infs_status infs20_paged_visit(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,infs20_tree_visitor visit,void *context)
{
    struct infs20_paged_state s;
    infs_status st=infs20_paged_open(l,io,&s); if(st) return st;
    struct reader r={l,io,&s}; struct infs20_tree_io tree=tree_reader(&r);
    return infs20_tree_visit(&tree,&s.root,visit,context);
}
infs_status infs20_paged_visit_range(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,uint64_t expected_sequence,
    const uint8_t *start,size_t start_size,int exclusive,
    const uint8_t *end,size_t end_size,size_t limit,infs20_tree_visitor visit,
    void *context,size_t *seen,int *more,uint64_t *sequence)
{
    if(!sequence) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_paged_state s;
    infs_status st=infs20_paged_open(l,io,&s); if(st) return st;
    /* Never resume into a different committed namespace. A caller that
     * does not hold an exclusive pin must restart after a sequence change. */
    if(expected_sequence && expected_sequence!=s.commit.sequence)
        return INFS_STATUS_BUSY;
    struct reader r={l,io,&s}; struct infs20_tree_io tree=tree_reader(&r);
    st=infs20_tree_visit_range(&tree,&s.root,start,start_size,exclusive,end,
                               end_size,limit,visit,context,seen,more);
    if(!st) *sequence=s.commit.sequence;
    return st;
}
infs_status infs20_paged_read_view(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,infs20_paged_view_fn visit,void *context)
{
    if(!visit) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_paged_state s;
    infs_status st=infs20_paged_open(l,io,&s); if(st) return st;
    struct reader r={l,io,&s}; struct infs20_tree_io tree=tree_reader(&r);
    return visit(context,&tree,&s.root,s.commit.sequence);
}
/* Both normal bounded discovery and explicit offline discovery use
 * this single committed descriptor decoder and native tree validator. */
static infs_status open_discovered(struct infs20_store_discovery *d,
    struct infs20_store_region *region,struct infs20_paged_layout *layout,
    struct infs20_paged_state *out,int full_tree)
{
    struct infs20_store_io io=infs20_store_region_io(&d->region);
    struct infs20_paged_layout l={0}; l.bootstrap=d->region.layout;
    infs_status st;
    struct infs20_paged_state s={0};
    uint8_t p[DESCRIPTOR_BYTES];
    st=descriptor(&l.bootstrap,&io,p,&s.commit); if(st) return st;
    if(memcmp(p,"P20A",4) || infs_load_le16(p+4)!=3)
        return INFS_STATUS_NOT_SUPPORTED;
    for(unsigned i=0;i<3;++i) {
        l.arena_start[i]=infs_load_le64(p+8+8*i);
        l.arena_blocks[i]=infs_load_le64(p+32+8*i);
    }
    if(!valid_layout(&l,&io,0)) return INFS_STATUS_CORRUPT;
    st=decode(&l,p,&s); if(st) return st;
    /* Offline discovery explicitly verifies the complete tree. Ordinary
     * indexed access must not traverse the whole namespace at mount. */
    if(full_tree) {
        struct reader r={&l,&io,&s}; struct infs20_tree_io tree=tree_reader(&r);
        st=infs20_tree_visit(&tree,&s.root,NULL,NULL);
        if(st)return st;
    } else {
        struct infs20_paged_state opened;
        st=infs20_paged_open(&l,&io,&opened);
        if(st)return st;
        s=opened;
    }
    *region=d->region; *layout=l; *out=s;
    return INFS_STATUS_OK;
}
infs_status infs20_paged_discover(const struct infs20_raw_source *source,
    struct infs20_store_region *region,struct infs20_paged_layout *layout,
    struct infs20_paged_state *out)
{
    if(!region || !layout || !out)return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_store_discovery d;
    infs_status st=infs20_store_discover(source,&d);
    return st?st:open_discovered(&d,region,layout,out,1);
}
infs_status infs20_paged_open_bounded(const struct infs20_raw_source *source,
    const uint64_t lane_start[INFS20_LANE_COUNT],
    struct infs20_store_region *region,struct infs20_paged_layout *layout,
    struct infs20_paged_state *out)
{
    if(!region || !layout || !out)return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_store_discovery d;
    infs_status st=infs20_store_open_bounded(source,lane_start,&d);
    return st?st:open_discovered(&d,region,layout,out,0);
}

/* Explicit mark/retire collection. Only current-root reachability matters
 * for the new free inventory because the same root is published again:
 * the existing current bank remains untouched; the previous bank is
 * overwritten completely before successful return. A cut before that point
 * exposes either the old allocator or a degraded new authority which the
 * mutator already refuses. No arena page is written during collection. */
infs_status infs20_paged_collect(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],struct infs20_paged_state *out)
{
    if(!out || !tx || zero(tx,16) || !valid_layout(l,io,1))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_paged_state s;
    infs_status st=infs20_paged_open(l,io,&s);
    if(st) return st;
    if(!s.commit.fully_protected || !memcmp(tx,s.commit.transaction_id,16))
        return INFS_STATUS_BUSY;
    if(s.commit.sequence==UINT64_MAX) return INFS_STATUS_OVERFLOW;
    struct reader r={l,io,&s};
    struct marks m={l,&s,io,NULL,0,0};
    st=trace_root(&r,&s.root,s.used[0],&m);
    if(!st) {
        /* The previous bank will now contain this same root. The frozen
         * proof itself consumes no new pages, even on a full arena. */
        s.reclaim_root=s.root; s.reclaim_epoch=s.commit.sequence+1;
        s.reclaim_limit=s.used[0]; s.reclaim_cursor=0;
        s.free_slots=s.used[0]-(uint64_t)m.count;
    }
    free(m.slots);
    if(!st) st=publish(l,io,tx,&s,NULL,NULL,NULL,NULL);
    if(!st) *out=s;
    return st;
}
