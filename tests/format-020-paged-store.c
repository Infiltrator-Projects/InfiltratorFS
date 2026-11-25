// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/paged_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-paged-store:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { LANE=384, BLOCKS=3*LANE, COUNT=48, KEY=700, VALUE=500 };
struct medium {
    uint8_t *disk,*pending;
    unsigned ops,fail,mode,writes;
};
static const size_t bytes=(size_t)BLOCKS*INFS20_PAGE_BYTES;
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o; if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES);
    return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o; if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    ++m->writes;
    if(++m->ops==m->fail) {
        /* Simulate a device reporting success but dropping a page write. */
        if(m->mode==3) return INFS_STATUS_OK;
        if(m->mode) {
            size_t n=m->mode==1?INFS20_PAGE_BYTES/2:INFS20_PAGE_BYTES;
            memcpy(m->disk+b*INFS20_PAGE_BYTES,p,n);
            memcpy(m->pending+b*INFS20_PAGE_BYTES,p,n);
        }
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->pending+b*INFS20_PAGE_BYTES,p,INFS20_PAGE_BYTES);
    return 0;
}
static infs_status fl(void *o)
{
    struct medium *m=o;
    if(++m->ops==m->fail) {
        if(m->mode==2) memcpy(m->disk,m->pending,bytes);
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,bytes); return 0;
}
static void cold(struct medium *m)
{
    memcpy(m->pending,m->disk,bytes); m->ops=m->fail=0;
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[15]=0x71;
}
static void key(unsigned n,uint8_t p[KEY])
{
    memset(p,(int)n,KEY); p[0]=(uint8_t)n;
}
static void value(unsigned n,uint8_t p[VALUE])
{
    for(unsigned i=0;i<VALUE;++i) p[i]=(uint8_t)(n*7+i*11);
}
static int expect(const struct infs20_paged_layout *l,const struct infs20_store_io *io,
                  unsigned n,const uint8_t *expected)
{
    uint8_t k[KEY],v[VALUE],out[VALUE]; size_t size=0;
    key(n,k); value(n,v);
    return !infs20_paged_get(l,io,k,KEY,out,VALUE,&size) && size==VALUE &&
           !memcmp(out,expected?expected:v,VALUE);
}
struct walk { unsigned n; };
static infs_status visit(void *opaque,const uint8_t *k,size_t ks,const uint8_t *v,size_t vs)
{
    struct walk *w=opaque; uint8_t ek[KEY],ev[VALUE];
    key(w->n,ek); value(w->n,ev);
    if(ks!=KEY || vs!=VALUE || memcmp(k,ek,KEY) || memcmp(v,ev,VALUE))
        return INFS_STATUS_CORRUPT;
    ++w->n; return 0;
}
struct raw { uint8_t *p; size_t n; };
static infs_status raw_read(void *o,uint64_t off,uint8_t *p,size_t n)
{
    struct raw *r=o;
    if(off>r->n || n>r->n-(size_t)off) return INFS_STATUS_IO_ERROR;
    memcpy(p,r->p+(size_t)off,n); return 0;
}
int main(void)
{
    struct medium m={0};
    m.disk=calloc(1,bytes); m.pending=calloc(1,bytes);
    uint8_t *baseline=malloc(bytes); CHECK(m.disk && m.pending && baseline);
    struct infs20_paged_layout l={
        {{1},{2},{3},BLOCKS,{0,LANE,2*LANE}},
        {INFS20_STORE_LANE_BLOCKS,LANE+INFS20_STORE_LANE_BLOCKS,2*LANE+INFS20_STORE_LANE_BLOCKS},
        {LANE-INFS20_STORE_LANE_BLOCKS,LANE-INFS20_STORE_LANE_BLOCKS,LANE-INFS20_STORE_LANE_BLOCKS}};
    struct infs20_store_io io={&m,rd,wr,fl};
    struct infs20_paged_state s;
    uint8_t id[16],k[KEY],v[VALUE],changed[VALUE];
    tx(id,1); CHECK(!infs20_paged_create(&l,&io,id,&s)); cold(&m);
    CHECK(s.commit.sequence==1 && !s.root.sequence);
    tx(id,2);
    CHECK(infs20_paged_create(&l,&io,id,&s)==INFS_STATUS_ALREADY_EXISTS && !m.ops);
    for(unsigned i=0;i<COUNT;++i) {
        key(i,k); value(i,v); tx(id,i+3);
        CHECK(!infs20_paged_put(&l,&io,id,k,KEY,v,VALUE,&s)); cold(&m);
    }
    CHECK(s.commit.sequence==COUNT+1 && s.commit.records==1);
    CHECK(s.used[0]==s.used[1] && s.used[1]==s.used[2]);
    struct walk w={0}; CHECK(!infs20_paged_visit(&l,&io,visit,&w) && w.n==COUNT);
    for(unsigned i=0;i<COUNT;++i) CHECK(expect(&l,&io,i,NULL));
    memcpy(baseline,m.disk,bytes); struct infs20_paged_state original=s;
    memset(changed,0x86,sizeof(changed)); key(17,k); tx(id,100);
    CHECK(!infs20_paged_put(&l,&io,id,k,KEY,changed,VALUE,&s));
    unsigned cuts=m.ops; cold(&m);
    CHECK(expect(&l,&io,17,changed));
    CHECK(s.used[0]-original.used[0]<8);
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=cuts;++cut) {
        memcpy(m.disk,baseline,bytes); cold(&m);
        m.mode=mode; m.fail=cut; tx(id,200+mode*100+cut);
        memset(&s,0xa5,sizeof(s)); uint8_t prior[sizeof(s)]; memcpy(prior,&s,sizeof(s));
        CHECK(infs20_paged_put(&l,&io,id,k,KEY,changed,VALUE,&s)==INFS_STATUS_IO_ERROR);
        CHECK(!memcmp(prior,&s,sizeof(s)));
        cold(&m); CHECK(!infs20_paged_open(&l,&io,&s));
        CHECK(s.commit.sequence==original.commit.sequence ||
              s.commit.sequence==original.commit.sequence+1);
        CHECK(!infs20_paged_visit(&l,&io,NULL,NULL));
        CHECK(expect(&l,&io,17,s.commit.sequence==original.commit.sequence?NULL:changed));
        CHECK(expect(&l,&io,0,NULL) && expect(&l,&io,COUNT-1,NULL));
        ++cases;
    }
    /* Damage a single current replica. Read-only membership still works;
     * mutation must refuse without a payload/metadata write. */
    memcpy(m.disk,baseline,bytes); cold(&m);
    memset(m.disk+original.root.blocks[1]*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    cold(&m); tx(id,898);
    CHECK(expect(&l,&io,17,NULL));
    CHECK(infs20_paged_put(&l,&io,id,k,KEY,changed,VALUE,&s)==INFS_STATUS_BUSY);
    CHECK(!m.ops);
    /* A silently dropped new leaf write must not publish new authority. */
    memcpy(m.disk,baseline,bytes); cold(&m);
    m.fail=1; m.mode=3; tx(id,899);
    CHECK(infs20_paged_put(&l,&io,id,k,KEY,changed,VALUE,&s)!=INFS_STATUS_OK);
    cold(&m); CHECK(!infs20_paged_open(&l,&io,&s));
    CHECK(s.commit.sequence==original.commit.sequence && expect(&l,&io,17,NULL));
    memcpy(m.disk,baseline,bytes); cold(&m);
    tx(id,900); key(17,k); CHECK(!infs20_paged_remove(&l,&io,id,k,KEY,&s)); cold(&m);
    size_t size=99; uint8_t out[VALUE]; memset(out,0xa5,sizeof(out));
    CHECK(infs20_paged_get(&l,&io,k,KEY,out,VALUE,&size)==INFS_STATUS_NOT_FOUND);
    CHECK(size==99 && out[0]==0xa5);
    CHECK(!infs20_paged_visit(&l,&io,NULL,NULL));
    /* Two entire placement domains lost, including both bootstrap banks
     * and every tree page allocated in those lanes. One surviving copy
     * still proves the current namespace and physical geometry. */
    memset(m.disk,0,2*(size_t)LANE*INFS20_PAGE_BYTES); cold(&m);
    CHECK(!infs20_paged_visit(&l,&io,NULL,NULL) && expect(&l,&io,0,NULL));
    struct raw raw={calloc(1,512+bytes),512+bytes}; CHECK(raw.p);
    memcpy(raw.p+512,m.disk,bytes);
    struct infs20_raw_source source={&raw,raw.n,raw_read};
    struct infs20_store_region region; struct infs20_paged_layout found;
    struct infs20_paged_state recovered;
    CHECK(!infs20_paged_discover(&source,&region,&found,&recovered));
    CHECK(region.start==512 && recovered.commit.sequence==s.commit.sequence &&
          !recovered.commit.fully_protected);
    struct infs20_store_io read_only=infs20_store_region_io(&region);
    CHECK(!read_only.write && !read_only.flush);
    CHECK(expect(&found,&read_only,COUNT-1,NULL));
    CHECK(!memcmp(raw.p+512,m.disk,bytes));
    /* Damage the only remaining current root. Bootstrap proof alone is
     * insufficient to claim a recovered filesystem. */
    memset(raw.p+512+recovered.root.blocks[2]*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    CHECK(infs20_paged_discover(&source,&region,&found,&recovered)==INFS_STATUS_CORRUPT);
    free(raw.p);
    /* Arena exhaustion has zero storage writes because reservations and
     * entire path preparation occur before the common publication. */
    memset(m.disk,0,bytes); cold(&m);
    for(unsigned i=0;i<3;++i) l.arena_blocks[i]=1;
    tx(id,901); CHECK(!infs20_paged_create(&l,&io,id,&s)); cold(&m);
    tx(id,902); key(0,k); value(0,v);
    CHECK(!infs20_paged_put(&l,&io,id,k,KEY,v,VALUE,&s)); cold(&m);
    tx(id,903); unsigned writes=m.writes;
    CHECK(infs20_paged_put(&l,&io,id,k,KEY,changed,VALUE,&s)==INFS_STATUS_NO_SPACE);
    CHECK(m.writes==writes && expect(&l,&io,0,NULL));
    free(baseline); free(m.pending); free(m.disk);
    printf("format20-paged-store: PASS (%u committed keys, %u actual publication interruption cases)\n",COUNT,cases);
    return 0;
}
