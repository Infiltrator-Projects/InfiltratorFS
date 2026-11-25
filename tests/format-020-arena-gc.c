// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/paged_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-arena-gc:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { LANE=32, BLOCKS=3*LANE };
static const size_t bytes=(size_t)BLOCKS*INFS20_PAGE_BYTES;
struct media { uint8_t *disk,*pending; unsigned ops,fail,mode,arena_writes; };
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct media *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct media *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    if(b%LANE>=20) ++m->arena_writes;
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
        if(m->mode==2) memcpy(m->disk,m->pending,bytes);
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,bytes); return 0;
}
static void cold(struct media *m)
{
    memcpy(m->pending,m->disk,bytes); m->ops=m->fail=m->arena_writes=0;
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
int main(void)
{
    struct media m={0};
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
    cold(&m); CHECK(s.commit.sequence==4 && s.free_count==1 &&
        s.free_runs[0].start==0 && s.free_runs[0].blocks==1 &&
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
    CHECK(s.used[0]==2 && !s.free_count && s.root.blocks[0]==20 && expect(&l,&io,3));
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
        CHECK(s.free_count==1);
    }
    /* Missing committed proof must prevent the collector from inventing
     * free space; read/verification failure is nonmutating. */
    for(unsigned lane=0;lane<3;++lane)
        memset(m.disk+s.root.blocks[lane]*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    cold(&m); tx(id,3000);
    CHECK(infs20_paged_collect(&l,&io,id,&s)==INFS_STATUS_CORRUPT && !m.ops);
    free(retired); free(base); free(m.pending); free(m.disk);
    printf("format20-arena-gc: PASS (%u interruption cases, 32 bounded reuse cycles)\n",cases);
    return 0;
}
