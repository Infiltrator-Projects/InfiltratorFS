// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/paged_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-arena-gc:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { LANE=32, BLOCKS=3*LANE };
static const size_t bytes=(size_t)BLOCKS*INFS20_PAGE_BYTES;
struct media { uint8_t *disk,*pending; size_t bytes; unsigned lane,ops,fail,mode,arena_writes; };
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct media *m=o;
    if(b>=m->bytes/INFS20_PAGE_BYTES) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct media *m=o;
    if(b>=m->bytes/INFS20_PAGE_BYTES) return INFS_STATUS_IO_ERROR;
    if(b%m->lane>=20) ++m->arena_writes;
    if(++m->ops==m->fail) {
        if(m->mode) {
            size_t n=m->mode==1?INFS20_PAGE_BYTES/2:INFS20_PAGE_BYTES;
            memcpy(m->disk+b*INFS20_PAGE_BYTES,p,n);
            memcpy(m->pending+b*INFS20_PAGE_BYTES,p,n);
        }
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->pending+b*INFS20_PAGE_BYTES,p,INFS20_PAGE_BYTES); return 0;
}
static infs_status fl(void *o)
{
    struct media *m=o;
    if(++m->ops==m->fail) {
        if(m->mode==2) memcpy(m->disk,m->pending,m->bytes);
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,m->bytes); return 0;
}
static void cold(struct media *m)
{
    memcpy(m->pending,m->disk,m->bytes); m->ops=m->fail=m->arena_writes=0;
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[15]=0x57;
}
static infs_status put(const struct infs20_paged_layout *l,const struct infs20_store_io *io,
    const uint8_t id[16],uint8_t value,struct infs20_paged_state *s)
{
    return infs20_paged_put(l,io,id,(const uint8_t *)"key",3,&value,1,s);
}
static int expect(const struct infs20_paged_layout *l,const struct infs20_store_io *io,uint8_t expected)
{
    uint8_t out=0; size_t size=0;
    return !infs20_paged_get(l,io,(const uint8_t *)"key",3,&out,1,&size) &&
           size==1 && out==expected;
}
/* Record live relative slots from a validated committed tree. This is an
 * independent test oracle for fragmented free space and reuse. */
struct trace { uint64_t arena; uint8_t *live; size_t slots,count; };
static infs_status visit_ref(void *opaque,const struct infs20_tree_ref *ref)
{
    struct trace *t=opaque;
    if(ref->blocks[0]<t->arena || ref->blocks[0]-t->arena>=t->slots)
        return INFS_STATUS_CORRUPT;
    size_t slot=(size_t)(ref->blocks[0]-t->arena);
    if(t->live[slot]) return INFS_STATUS_CORRUPT;
    t->live[slot]=1; ++t->count; return INFS_STATUS_OK;
}
static infs_status trace_view(void *opaque,const struct infs20_tree_io *tree,
    const struct infs20_tree_ref *root,uint64_t sequence)
{
    (void)sequence;
    return infs20_tree_visit_refs(tree,root,visit_ref,opaque);
}
static int fragmented_reuse(void)
{
    enum { N=170, BIG_LANE=2048, BIG_BLOCKS=3*BIG_LANE, VALUE=3000 };
    struct media m={0}; m.bytes=(size_t)BIG_BLOCKS*INFS20_PAGE_BYTES; m.lane=BIG_LANE;
    m.disk=calloc(1,m.bytes); m.pending=calloc(1,m.bytes); CHECK(m.disk && m.pending);
    struct infs20_paged_layout l={{{1},{2},{3},BIG_BLOCKS,{0,BIG_LANE,2*BIG_LANE}},
        {20,BIG_LANE+20,2*BIG_LANE+20},{2000,2000,2000}};
    struct infs20_store_io io={&m,rd,wr,fl};
    struct infs20_paged_change changes[N]; char keys[N][5]; uint8_t value[VALUE];
    memset(value,0x6d,sizeof(value));
    for(unsigned i=0;i<N;++i) {
        CHECK(snprintf(keys[i],sizeof(keys[i]),"%04u",i)==4);
        changes[i]=(struct infs20_paged_change){(const uint8_t *)keys[i],value,4,sizeof(value),0};
    }
    uint8_t id[16]; struct infs20_paged_state s;
    tx(id,4000); CHECK(!infs20_paged_apply(&l,&io,id,changes,N,1,NULL,NULL,NULL,&s)); cold(&m);
    uint64_t used=s.used[0]; CHECK(used>0 && used<=2000);
    /* Rebuild with exactly the measured append capacity so recycling is
     * mandatory. Geometry does not influence the tree splitting policy. */
    memset(m.disk,0,m.bytes); cold(&m);
    for(unsigned lane=0;lane<3;++lane) l.arena_blocks[lane]=used;
    tx(id,4001); CHECK(!infs20_paged_apply(&l,&io,id,changes,N,1,NULL,NULL,NULL,&s)); cold(&m);
    CHECK(s.used[0]==used);
    uint8_t *live=calloc((size_t)used,1); CHECK(live);
    struct trace t={l.arena_start[0],live,(size_t)used,0};
    CHECK(!infs20_paged_read_view(&l,&io,trace_view,&t));
    size_t gaps=0;
    for(size_t i=0;i<t.slots;++i) if(!live[i] && (!i || live[i-1])) ++gaps;
    CHECK(gaps>128);
    tx(id,4002); CHECK(!infs20_paged_collect(&l,&io,id,&s));
    CHECK(!m.arena_writes && s.free_slots==used-t.count); cold(&m);
    uint64_t free_before=s.free_slots;
    tx(id,4003); value[0]=0x91;
    CHECK(!infs20_paged_put(&l,&io,id,(const uint8_t *)keys[0],4,value,sizeof(value),&s));
    CHECK(s.used[0]==used && s.free_slots<free_before && s.reclaim_cursor>0);
    cold(&m);
    uint8_t out[VALUE]; size_t n=0;
    CHECK(!infs20_paged_get(&l,&io,(const uint8_t *)keys[0],4,out,sizeof(out),&n) &&
          n==sizeof(out) && !memcmp(out,value,n));
    value[0]=0x6d;
    for(unsigned i=1;i<N;++i) {
        CHECK(!infs20_paged_get(&l,&io,(const uint8_t *)keys[i],4,out,sizeof(out),&n) &&
              n==sizeof(out) && !memcmp(out,value,n));
    }
    /* Frozen root's physical pages remain unchanged even when superseded. */
    CHECK(s.reclaim_epoch==2);
    tx(id,4004); CHECK(!infs20_paged_collect(&l,&io,id,&s)); cold(&m);
    CHECK(s.reclaim_epoch==4 && s.reclaim_cursor==0);
    free(live); free(m.pending); free(m.disk); return 0;
}
int main(void)
{
    struct media m={0}; m.bytes=bytes; m.lane=LANE;
    m.disk=calloc(1,bytes); m.pending=calloc(1,bytes);
    uint8_t *base=malloc(bytes),*retired=malloc(bytes);
    CHECK(m.disk && m.pending && base && retired);
    struct infs20_paged_layout l={{{1},{2},{3},BLOCKS,{0,LANE,2*LANE}},
        {20,LANE+20,2*LANE+20},{2,2,2}};
    struct infs20_store_io io={&m,rd,wr,fl};
    uint8_t id[16]; struct infs20_paged_state s;
    tx(id,1); CHECK(!infs20_paged_create(&l,&io,id,&s)); cold(&m);
    tx(id,2); CHECK(!put(&l,&io,id,1,&s)); cold(&m);
    tx(id,3); CHECK(!put(&l,&io,id,2,&s)); cold(&m);
    CHECK(s.used[0]==2 && s.root.blocks[0]==21 && expect(&l,&io,2));
    tx(id,4); CHECK(put(&l,&io,id,3,&s)==INFS_STATUS_NO_SPACE && !m.ops);
    memcpy(base,m.disk,bytes);
    tx(id,5); CHECK(!infs20_paged_collect(&l,&io,id,&s));
    unsigned cuts=m.ops; CHECK(!m.arena_writes);
    cold(&m); CHECK(s.commit.sequence==4 && s.free_slots==1 &&
        s.reclaim_cursor==0 && s.reclaim_limit==2 &&
        s.root.blocks[0]==21 && expect(&l,&io,2));
    memcpy(retired,m.disk,bytes);
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=cuts;++cut) {
        memcpy(m.disk,base,bytes); cold(&m); m.mode=mode; m.fail=cut;
        tx(id,100+mode*100+cut);
        CHECK(infs20_paged_collect(&l,&io,id,&s)==INFS_STATUS_IO_ERROR);
        CHECK(!m.arena_writes); cold(&m);
        CHECK(!infs20_paged_open(&l,&io,&s));
        CHECK((s.commit.sequence==3 || s.commit.sequence==4) && expect(&l,&io,2));
        if(s.commit.sequence==3 || !s.commit.fully_protected) {
            tx(id,500+mode*100+cut);
            infs_status st=put(&l,&io,id,3,&s);
            CHECK((st==INFS_STATUS_NO_SPACE || st==INFS_STATUS_BUSY) && !m.ops);
        }
        ++cases;
    }
    memcpy(m.disk,retired,bytes); cold(&m);
    tx(id,900); CHECK(!put(&l,&io,id,3,&s));
    cuts=m.ops; cold(&m);
    CHECK(s.used[0]==2 && !s.free_slots && s.root.blocks[0]==20 && expect(&l,&io,3));
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=cuts;++cut) {
        memcpy(m.disk,retired,bytes); cold(&m); m.mode=mode; m.fail=cut;
        tx(id,1000+mode*100+cut);
        CHECK(put(&l,&io,id,3,&s)==INFS_STATUS_IO_ERROR); cold(&m);
        CHECK(!infs20_paged_open(&l,&io,&s));
        CHECK((s.commit.sequence==4 || s.commit.sequence==5) &&
              expect(&l,&io,s.commit.sequence==4?2:3));
        ++cases;
    }
    /* Repeated collection alternates the same two physical slots; no
     * append capacity remains and every current value must survive. */
    memcpy(m.disk,retired,bytes); cold(&m);
    for(unsigned i=0;i<32;++i) {
        tx(id,2000+i*2); CHECK(!put(&l,&io,id,(uint8_t)(i+3),&s)); cold(&m);
        CHECK(expect(&l,&io,(uint8_t)(i+3)) && s.used[0]==2);
        tx(id,2001+i*2); CHECK(!infs20_paged_collect(&l,&io,id,&s)); cold(&m);
        CHECK(s.free_slots==1);
    }
    /* One missing tree copy is still readable, but does not meet the
     * protection contract required to retire a preceding bank. */
    memset(m.disk+s.root.blocks[0]*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    cold(&m); tx(id,2999);
    CHECK(expect(&l,&io,34));
    CHECK(infs20_paged_collect(&l,&io,id,&s)==INFS_STATUS_BUSY && !m.ops);
    /* Missing committed proof must prevent the collector from inventing
     * free space; read/verification failure is nonmutating. */
    for(unsigned lane=0;lane<3;++lane)
        memset(m.disk+s.root.blocks[lane]*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    cold(&m); tx(id,3000);
    CHECK(infs20_paged_collect(&l,&io,id,&s)==INFS_STATUS_CORRUPT && !m.ops);
    free(retired); free(base); free(m.pending); free(m.disk);
    CHECK(!fragmented_reuse());
    printf("format20-arena-gc: PASS (%u interruption cases, 32 bounded reuse cycles)\n",cases);
    return 0;
}
