// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/paged_store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-batch:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { BLOCKS=192, LANE=64, DATA=63 };
static const size_t bytes=(size_t)BLOCKS*INFS20_PAGE_BYTES;
struct medium { uint8_t *disk,*pending; size_t bytes; unsigned ops,fail,mode,calls; };
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o; if(b>=m->bytes/INFS20_PAGE_BYTES) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o; if(b>=m->bytes/INFS20_PAGE_BYTES) return INFS_STATUS_IO_ERROR;
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
    struct medium *m=o;
    if(++m->ops==m->fail) {
        if(m->mode==2) memcpy(m->disk,m->pending,m->bytes);
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,m->bytes); return 0;
}
static void cold(struct medium *m)
{
    memcpy(m->pending,m->disk,m->bytes); m->ops=m->fail=m->calls=0;
}
static infs_status payload(void *o)
{
    struct medium *m=o; uint8_t page[INFS20_PAGE_BYTES];
    ++m->calls; memset(page,0x6d,sizeof(page)); return wr(m,DATA,page);
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[15]=0x6a;
}
static int value(const struct infs20_paged_layout *l,const struct infs20_store_io *io,
                  const char *key,const char *want)
{
    uint8_t out[16]; memset(out,0xa5,sizeof(out)); size_t n=99;
    infs_status st=infs20_paged_get(l,io,(const uint8_t *)key,strlen(key),out,sizeof(out),&n);
    return want?(!st && n==strlen(want) && !memcmp(out,want,n)):
                (st==INFS_STATUS_NOT_FOUND && n==99 && out[0]==0xa5);
}
#define PUT(k,v) {(const uint8_t *)(k),(const uint8_t *)(v),sizeof(k)-1,sizeof(v)-1,0}
#define DEL(k) {(const uint8_t *)(k),NULL,sizeof(k)-1,0,1}
/* Exceed the former fixed 256-entry limit for both insertion and removal.
 * The same immutable commit authority must expose exactly one successor. */
static int large_batch(void)
{
    enum { N=300, BIG_LANE=1100, BIG_BLOCKS=3*BIG_LANE };
    struct medium m={0}; m.bytes=(size_t)BIG_BLOCKS*INFS20_PAGE_BYTES;
    m.disk=calloc(1,m.bytes); m.pending=calloc(1,m.bytes);
    CHECK(m.disk && m.pending);
    struct infs20_paged_layout l={{{1},{2},{3},BIG_BLOCKS,{0,BIG_LANE,2*BIG_LANE}},
        {20,BIG_LANE+20,2*BIG_LANE+20},{1080,1080,1080}};
    struct infs20_store_io io={&m,rd,wr,fl};
    struct infs20_store_record records[N];
    struct infs20_paged_change changes[N+1];
    char keys[N][5];
    for(unsigned i=0;i<N;++i) {
        CHECK(snprintf(keys[i],sizeof(keys[i]),"%04u",i)==4);
        records[i]=(struct infs20_store_record){(const uint8_t *)keys[i],(const uint8_t *)"a",4,1};
        changes[i]=(struct infs20_paged_change){records[i].key,(const uint8_t *)"b",4,1,0};
    }
    struct infs20_paged_state s;
    uint8_t id[16]; tx(id,601);
    CHECK(!infs20_paged_replace(&l,&io,id,records,N,1,NULL,NULL,NULL,&s));
    CHECK(s.commit.sequence==1); cold(&m);
    for(unsigned i=0;i<N;++i) CHECK(value(&l,&io,keys[i],"a"));
    /* A late refusal must not write any of the 300 staged edits. */
    changes[N]=(struct infs20_paged_change){(const uint8_t *)"missing",NULL,7,0,1};
    tx(id,602);
    CHECK(infs20_paged_apply(&l,&io,id,changes,N+1,0,payload,NULL,&m,&s)==INFS_STATUS_NOT_FOUND);
    CHECK(!m.ops && !m.calls && s.commit.sequence==1);
    tx(id,603);
    CHECK(!infs20_paged_apply(&l,&io,id,changes,N,0,NULL,NULL,NULL,&s));
    CHECK(s.commit.sequence==2); cold(&m);
    for(unsigned i=0;i<N;++i) CHECK(value(&l,&io,keys[i],"b"));
    tx(id,604);
    CHECK(!infs20_paged_replace(&l,&io,id,NULL,0,0,NULL,NULL,NULL,&s));
    CHECK(s.commit.sequence==3); cold(&m);
    for(unsigned i=0;i<N;++i) CHECK(value(&l,&io,keys[i],NULL));
    free(m.pending); free(m.disk);
    return 0;
}
int main(void)
{
    struct medium m={0}; m.bytes=bytes; m.disk=calloc(1,bytes); m.pending=calloc(1,bytes);
    uint8_t *baseline=malloc(bytes); CHECK(m.disk && m.pending && baseline);
    struct infs20_paged_layout l={{{1},{2},{3},BLOCKS,{0,LANE,2*LANE}},
        {20,LANE+20,2*LANE+20},{40,40,40}};
    struct infs20_store_io io={&m,rd,wr,fl};
    struct infs20_paged_state s;
    uint8_t id[16]; tx(id,1);
    const struct infs20_paged_change initial[]={PUT("D","old"),PUT("F","old"),PUT("N","old")};
    CHECK(!infs20_paged_apply(&l,&io,id,initial,3,1,NULL,NULL,NULL,&s));
    CHECK(s.commit.sequence==1); cold(&m);
    CHECK(value(&l,&io,"D","old") && value(&l,&io,"F","old") && value(&l,&io,"N","old"));
    memcpy(baseline,m.disk,bytes);
    const struct infs20_paged_change changes[]={PUT("D","new"),PUT("F","new"),DEL("N"),PUT("E","new")};
    tx(id,2); CHECK(!infs20_paged_apply(&l,&io,id,changes,4,0,payload,NULL,&m,&s));
    CHECK(m.calls==1 && s.commit.sequence==2); unsigned cuts=m.ops; cold(&m);
    CHECK(value(&l,&io,"D","new") && value(&l,&io,"F","new") &&
          value(&l,&io,"N",NULL) && value(&l,&io,"E","new"));
    CHECK(m.disk[(size_t)DATA*INFS20_PAGE_BYTES]==0x6d);
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=cuts;++cut) {
        memcpy(m.disk,baseline,bytes); cold(&m); m.mode=mode; m.fail=cut;
        tx(id,100+mode*100+cut); memset(&s,0xa5,sizeof(s));
        uint8_t prior[sizeof(s)]; memcpy(prior,&s,sizeof(prior));
        CHECK(infs20_paged_apply(&l,&io,id,changes,4,0,payload,NULL,&m,&s)==INFS_STATUS_IO_ERROR);
        CHECK(!memcmp(prior,&s,sizeof(s)));
        cold(&m); CHECK(!infs20_paged_open(&l,&io,&s));
        CHECK(s.commit.sequence==1 || s.commit.sequence==2);
        CHECK(!infs20_paged_visit(&l,&io,NULL,NULL));
        if(s.commit.sequence==1)
            CHECK(value(&l,&io,"D","old") && value(&l,&io,"F","old") &&
                  value(&l,&io,"N","old") && value(&l,&io,"E",NULL));
        else CHECK(value(&l,&io,"D","new") && value(&l,&io,"F","new") &&
                  value(&l,&io,"N",NULL) && value(&l,&io,"E","new") &&
                  m.disk[(size_t)DATA*INFS20_PAGE_BYTES]==0x6d);
        ++cases;
    }
    memcpy(m.disk,baseline,bytes); cold(&m); tx(id,500);
    const struct infs20_paged_change invalid[]={PUT("D","new"),DEL("missing")};
    CHECK(infs20_paged_apply(&l,&io,id,invalid,2,0,payload,NULL,&m,&s)==INFS_STATUS_NOT_FOUND);
    CHECK(!m.ops && !m.calls && value(&l,&io,"D","old"));
    tx(id,501);
    CHECK(infs20_paged_apply(&l,&io,id,changes,4,1,payload,NULL,&m,&s)==INFS_STATUS_ALREADY_EXISTS);
    CHECK(!m.ops && !m.calls);
    free(baseline); free(m.pending); free(m.disk);
    CHECK(!large_batch());
    printf("format20-batch: PASS (%u multi-record/payload interruption cases; 300-record insert/update/removal and late refusal)\n",cases);
    return 0;
}
