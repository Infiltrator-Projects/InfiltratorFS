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
enum { ROOT_OFFSET=80, FREE_OFFSET=168,
       DESCRIPTOR_BYTES=FREE_OFFSET+16*INFS20_ARENA_FREE_MAX,
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
static int free_slot(const struct infs20_paged_state *s,uint64_t slot)
{
    for(size_t i=0;i<s->free_count;++i) {
        const struct infs20_arena_run *r=&s->free_runs[i];
        if(slot<r->start) return 0;
        if(slot-r->start<r->blocks) return 1;
    }
    return 0;
}
static int root_valid(const struct infs20_paged_layout *l,
                      const struct infs20_paged_state *s)
{
    if(empty_root(&s->root)) return 1;
    if(zero(s->root.id,16) || zero(s->root.digest,32) ||
       !s->root.sequence || s->root.sequence>s->commit.sequence) return 0;
    for(unsigned lane=0;lane<3;++lane)
        if(s->root.blocks[lane]<l->arena_start[lane] ||
           s->root.blocks[lane]-l->arena_start[lane]>=s->used[lane] ||
           s->root.blocks[lane]-l->arena_start[lane]!=
               s->root.blocks[0]-l->arena_start[0] ||
           free_slot(s,s->root.blocks[lane]-l->arena_start[lane]))
            return 0;
    return 1;
}
/* The fixed wire grammar is encoded fieldwise. No host struct padding. */
static void encode(const struct infs20_paged_layout *l,
    const struct infs20_paged_state *s,uint8_t p[DESCRIPTOR_BYTES])
{
    memset(p,0,DESCRIPTOR_BYTES);
    memcpy(p,"P20A",4); infs_store_le16(p+4,2);
    for(unsigned i=0;i<3;++i) {
        infs_store_le64(p+8+8*i,l->arena_start[i]);
        infs_store_le64(p+32+8*i,l->arena_blocks[i]);
        infs_store_le64(p+56+8*i,s->used[i]);
    }
    memcpy(p+ROOT_OFFSET,s->root.id,16);
    memcpy(p+ROOT_OFFSET+16,s->root.digest,32);
    infs_store_le64(p+ROOT_OFFSET+48,s->root.sequence);
    for(unsigned i=0;i<3;++i)
        infs_store_le64(p+ROOT_OFFSET+56+8*i,s->root.blocks[i]);
    infs_store_le16(p+160,(uint16_t)s->free_count);
    for(size_t i=0;i<s->free_count;++i) {
        infs_store_le64(p+FREE_OFFSET+16*i,s->free_runs[i].start);
        infs_store_le64(p+FREE_OFFSET+16*i+8,s->free_runs[i].blocks);
    }
}
static infs_status decode(const struct infs20_paged_layout *l,
    const uint8_t p[DESCRIPTOR_BYTES],struct infs20_paged_state *s)
{
    if(memcmp(p,"P20A",4) || infs_load_le16(p+4)!=2)
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
    memcpy(s->root.id,p+ROOT_OFFSET,16);
    memcpy(s->root.digest,p+ROOT_OFFSET+16,32);
    s->root.sequence=infs_load_le64(p+ROOT_OFFSET+48);
    for(unsigned i=0;i<3;++i)
        s->root.blocks[i]=infs_load_le64(p+ROOT_OFFSET+56+8*i);
    s->free_count=infs_load_le16(p+160);
    if(s->free_count>INFS20_ARENA_FREE_MAX || !zero(p+162,6))
        return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<s->free_count;++i) {
        struct infs20_arena_run *r=&s->free_runs[i];
        r->start=infs_load_le64(p+FREE_OFFSET+16*i);
        r->blocks=infs_load_le64(p+FREE_OFFSET+16*i+8);
        if(!r->blocks || r->start>=s->used[0] ||
           r->blocks>s->used[0]-r->start ||
           (i && s->free_runs[i-1].start+s->free_runs[i-1].blocks>=r->start))
            return INFS_STATUS_CORRUPT;
    }
    if(!zero(p+FREE_OFFSET+16*s->free_count,
             DESCRIPTOR_BYTES-FREE_OFFSET-16*s->free_count))
        return INFS_STATUS_CORRUPT;
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
    size_t size=0;
    st=infs20_store_lookup(b,io,descriptor_key,sizeof(descriptor_key),
                           p,DESCRIPTOR_BYTES,&size);
    if(st==INFS_STATUS_NOT_FOUND) return INFS_STATUS_NOT_SUPPORTED;
    if(st) return st;
    if(size==160 && !memcmp(p,"P20A",4) && infs_load_le16(p+4)==1)
        return INFS_STATUS_NOT_SUPPORTED;
    return size==DESCRIPTOR_BYTES?INFS_STATUS_OK:INFS_STATUS_CORRUPT;
}
static int permitted(const struct infs20_paged_layout *l,
    const struct infs20_paged_state *s,uint64_t block)
{
    for(unsigned lane=0;lane<3;++lane)
        if(block>=l->arena_start[lane] &&
           block-l->arena_start[lane]<s->used[lane] &&
           !free_slot(s,block-l->arena_start[lane])) return 1;
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
static infs_status publish(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    struct infs20_paged_state *s,infs20_store_prepare prepare,void *opaque)
{
    uint8_t p[DESCRIPTOR_BYTES]; encode(l,s,p);
    struct infs20_store_record record={descriptor_key,p,sizeof(descriptor_key),
                                       DESCRIPTOR_BYTES};
    struct infs20_store_state committed;
    infs_status st=infs20_store_publish_prepared(&l->bootstrap,io,tx,&record,1,
                                                prepare,opaque,&committed);
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
    return infs20_paged_apply(l,io,tx,NULL,0,1,NULL,NULL,out);
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
    if(!append && !m->state.free_count) return INFS_STATUS_NO_SPACE;
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
        /* Consume an already committed free slot in the private state.
         * On failure the selected descriptor still owns the old free list. */
        struct infs20_arena_run *r=&m->state.free_runs[0];
        uint64_t slot=r->start++;
        if(!--r->blocks) {
            --m->state.free_count;
            memmove(r,r+1,m->state.free_count*sizeof(*r));
        }
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
        size_t cap=m->capacity?m->capacity*2:6;
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
    infs20_store_prepare payload_prepare,void *payload_context,
    struct infs20_paged_state *out)
{
    if(!out || !tx || zero(tx,16) || !valid_layout(l,io,1) ||
       count>INFS20_PAGED_BATCH_MAX || (count && !changes) ||
       (create_empty!=0 && create_empty!=1))
        return INFS_STATUS_INVALID_ARGUMENT;
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
    m.payload_prepare=payload_prepare; m.payload_context=payload_context;
    infs_status st;
    if(create_empty) st=blank_bootstrap(l,io);
    else {
        st=infs20_paged_open(l,io,&m.state);
        if(!st && (!m.state.commit.fully_protected ||
                   !memcmp(tx,m.state.commit.transaction_id,16)))
            st=INFS_STATUS_BUSY;
    }
    if(st) return st;
    if(m.state.commit.sequence==UINT64_MAX) return INFS_STATUS_OVERFLOW;
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
    if(!st) st=publish(l,io,tx,&m.state,prepare,&m);
    if(!st) *out=m.state;
    free(m.pages);
    return st;
}
infs_status infs20_paged_put(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const uint8_t *key,
    size_t ks,const uint8_t *value,size_t vs,struct infs20_paged_state *out)
{
    const struct infs20_paged_change change={key,value,ks,vs,0};
    return infs20_paged_apply(l,io,tx,&change,1,0,NULL,NULL,out);
}
infs_status infs20_paged_remove(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const uint8_t *key,
    size_t ks,struct infs20_paged_state *out)
{
    const struct infs20_paged_change change={key,NULL,ks,0,1};
    return infs20_paged_apply(l,io,tx,&change,1,0,NULL,NULL,out);
}
struct replacement {
    const struct infs20_store_record *next;
    size_t count,index,changes;
    struct infs20_paged_change batch[INFS20_PAGED_BATCH_MAX];
    uint8_t *owned[INFS20_PAGED_BATCH_MAX];
};
static int record_keycmp(const uint8_t *a,size_t an,const uint8_t *b,size_t bn)
{
    size_t n=an<bn?an:bn;
    int cmp=memcmp(a,b,n);
    return cmp?cmp:(an>bn)-(an<bn);
}
static infs_status queue_put(struct replacement *d)
{
    if(d->changes==INFS20_PAGED_BATCH_MAX) return INFS_STATUS_NO_SPACE;
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
    if(d->changes==INFS20_PAGED_BATCH_MAX) return INFS_STATUS_NO_SPACE;
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
    infs20_store_prepare prepare_payload,void *context,struct infs20_paged_state *out)
{
    if(!out || !tx || zero(tx,16) || !valid_layout(l,io,1) ||
       count>INFS20_PAGED_BATCH_MAX || (count && !records) ||
       (create_empty!=0 && create_empty!=1)) return INFS_STATUS_INVALID_ARGUMENT;
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
                                  prepare_payload,context,out);
    for(size_t i=0;i<d->changes;++i) free(d->owned[i]);
    free(d);
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
infs_status infs20_paged_discover(const struct infs20_raw_source *source,
    struct infs20_store_region *region,struct infs20_paged_layout *layout,
    struct infs20_paged_state *out)
{
    if(!region || !layout || !out) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_store_discovery d;
    infs_status st=infs20_store_discover(source,&d); if(st) return st;
    struct infs20_store_io io=infs20_store_region_io(&d.region);
    struct infs20_paged_layout l={0}; l.bootstrap=d.region.layout;
    struct infs20_paged_state s={0};
    uint8_t p[DESCRIPTOR_BYTES];
    st=descriptor(&l.bootstrap,&io,p,&s.commit); if(st) return st;
    if(memcmp(p,"P20A",4) || infs_load_le16(p+4)!=2)
        return INFS_STATUS_NOT_SUPPORTED;
    for(unsigned i=0;i<3;++i) {
        l.arena_start[i]=infs_load_le64(p+8+8*i);
        l.arena_blocks[i]=infs_load_le64(p+32+8*i);
    }
    if(!valid_layout(&l,&io,0)) return INFS_STATUS_CORRUPT;
    st=decode(&l,p,&s); if(st) return st;
    struct reader r={&l,&io,&s}; struct infs20_tree_io tree=tree_reader(&r);
    st=infs20_tree_visit(&tree,&s.root,NULL,NULL);
    if(st) return st;
    *region=d.region; *layout=l; *out=s;
    return INFS_STATUS_OK;
}

/* Explicit mark/retire collection. Only current-root reachability matters
 * for the new free inventory because the same root is published again:
 * the existing current bank remains untouched; the previous bank is
 * overwritten completely before successful return. A cut before that point
 * exposes either the old allocator or a degraded new authority which the
 * mutator already refuses. No arena page is written during collection. */
struct marks {
    const struct infs20_paged_layout *layout;
    const struct infs20_paged_state *state;
    uint64_t *slots;
    size_t count,capacity;
};
static infs_status mark_ref(void *opaque,const struct infs20_tree_ref *r)
{
    struct marks *m=opaque;
    uint64_t slot=0;
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
        if(r->blocks[lane]<m->layout->arena_start[lane])
            return INFS_STATUS_CORRUPT;
        uint64_t n=r->blocks[lane]-m->layout->arena_start[lane];
        if(n>=m->state->used[lane] || free_slot(m->state,n) ||
           (lane && n!=slot)) return INFS_STATUS_CORRUPT;
        slot=n;
    }
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
static infs_status add_free(struct infs20_paged_state *s,uint64_t a,uint64_t b)
{
    if(a==b) return INFS_STATUS_OK;
    if(a>b) return INFS_STATUS_CORRUPT;
    if(s->free_count==INFS20_ARENA_FREE_MAX) return INFS_STATUS_NO_SPACE;
    s->free_runs[s->free_count++]=(struct infs20_arena_run){a,b-a};
    return INFS_STATUS_OK;
}
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
    struct infs20_tree_io tree=tree_reader(&r);
    struct marks m={l,&s,NULL,0,0};
    st=infs20_tree_visit_refs(&tree,&s.root,mark_ref,&m);
    if(st) { free(m.slots); return st; }
    if(m.count>1) qsort(m.slots,m.count,sizeof(*m.slots),slot_cmp);
    s.free_count=0;
    uint64_t cursor=0;
    for(size_t i=0;i<m.count && !st;++i) {
        if(m.slots[i]<cursor) { st=INFS_STATUS_CORRUPT; break; }
        st=add_free(&s,cursor,m.slots[i]);
        cursor=m.slots[i]+1;
    }
    if(!st) st=add_free(&s,cursor,s.used[0]);
    free(m.slots);
    if(!st) st=publish(l,io,tx,&s,NULL,NULL);
    if(!st) *out=s;
    return st;
}
