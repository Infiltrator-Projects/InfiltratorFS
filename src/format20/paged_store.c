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
 * Appending never overwrites a committed page. The old root and frontier
 * remain valid until the existing store atomically publishes a successor.
 * An interrupted private suffix may be abandoned after cold authority
 * selection; fresh transaction IDs keep page incarnations distinct.
 */
enum { DESCRIPTOR_BYTES=160, ROOT_OFFSET=80,
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
static int root_valid(const struct infs20_paged_layout *l,
                      const struct infs20_paged_state *s)
{
    if(empty_root(&s->root)) return 1;
    if(zero(s->root.id,16) || zero(s->root.digest,32) ||
       !s->root.sequence || s->root.sequence>s->commit.sequence) return 0;
    for(unsigned lane=0;lane<3;++lane)
        if(s->root.blocks[lane]<l->arena_start[lane] ||
           s->root.blocks[lane]-l->arena_start[lane]>=s->used[lane])
            return 0;
    return 1;
}
/* The fixed wire grammar is encoded fieldwise. No host struct padding. */
static void encode(const struct infs20_paged_layout *l,
    const struct infs20_paged_state *s,uint8_t p[DESCRIPTOR_BYTES])
{
    memset(p,0,DESCRIPTOR_BYTES);
    memcpy(p,"P20A",4); infs_store_le16(p+4,1);
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
}
static infs_status decode(const struct infs20_paged_layout *l,
    const uint8_t p[DESCRIPTOR_BYTES],struct infs20_paged_state *s)
{
    if(memcmp(p,"P20A",4) || infs_load_le16(p+4)!=1)
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
    return size==DESCRIPTOR_BYTES?INFS_STATUS_OK:INFS_STATUS_CORRUPT;
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
    for(unsigned i=0;i<3;++i)
        if(m->state.used[i]==m->layout->arena_blocks[i])
            return INFS_STATUS_NO_SPACE;
    /* IDs are transaction-scoped and never reused after uncertain write.
     * Fresh tx is a caller contract; the sequence additionally separates
     * successful generations. A private serial separates split pages. */
    uint8_t preimage[36]={'P','2','0','I'},digest[32];
    memcpy(preimage+4,m->tx,16);
    infs_store_le64(preimage+20,m->state.commit.sequence+1);
    infs_store_le64(preimage+28,++m->serial);
    infs_sha256(preimage,sizeof(preimage),digest);
    memcpy(id,digest,16);
    for(unsigned i=0;i<3;++i)
        blocks[i]=m->layout->arena_start[i]+m->state.used[i]++;
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
    if(memcmp(p,"P20A",4) || infs_load_le16(p+4)!=1)
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
