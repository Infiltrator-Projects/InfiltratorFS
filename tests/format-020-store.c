// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "format20-store:%d: %s\n", __LINE__, #x); return 1; } } while (0)
enum { BLOCKS=96, RECORDS=12 };
struct medium {
    uint8_t durable[BLOCKS][INFS20_PAGE_BYTES];
    uint8_t pending[BLOCKS][INFS20_PAGE_BYTES];
    unsigned operations, fail, mode;
};
static infs_status rd(void *opaque, uint64_t b, uint8_t *p)
{
    struct medium *m=opaque;
    if (b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending[b],INFS20_PAGE_BYTES);
    return 0;
}
static infs_status wr(void *opaque, uint64_t b, const uint8_t *p)
{
    struct medium *m=opaque;
    if (b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    if (++m->operations==m->fail) {
        if (m->mode==1) {
            memcpy(m->pending[b],p,INFS20_PAGE_BYTES/2);
            memcpy(m->durable[b],p,INFS20_PAGE_BYTES/2);
        } else if (m->mode==2) {
            memcpy(m->pending[b],p,INFS20_PAGE_BYTES);
            memcpy(m->durable[b],p,INFS20_PAGE_BYTES);
        }
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->pending[b],p,INFS20_PAGE_BYTES);
    return 0;
}
static infs_status fl(void *opaque)
{
    struct medium *m=opaque;
    if (++m->operations==m->fail) {
        if (m->mode==2) memcpy(m->durable,m->pending,sizeof(m->durable));
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->durable,m->pending,sizeof(m->durable));
    return 0;
}
static void cold(struct medium *m)
{
    memcpy(m->pending,m->durable,sizeof(m->durable));
    m->fail=0; m->operations=0;
}
static void txid(uint8_t id[16], unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[2]=0xa5;
}
static uint64_t block(const struct infs20_store_layout *l,
                      unsigned lane, unsigned seq, unsigned slot)
{
    return l->lane_start[lane]+(seq&1u)*INFS20_STORE_BANK_BLOCKS+slot;
}
static void erase(struct medium *m, uint64_t b)
{
    memset(m->durable[b],0,INFS20_PAGE_BYTES);
    memset(m->pending[b],0,INFS20_PAGE_BYTES);
}
static int verify_values(const struct infs20_store_layout *l,
                         const struct infs20_store_io *io, uint8_t expected)
{
    for (unsigned i=0;i<RECORDS;++i) {
        uint8_t key=(uint8_t)(i+1), value[1000]; size_t n=0;
        CHECK(infs20_store_lookup(l,io,&key,1,value,sizeof(value),&n)==0);
        CHECK(n==sizeof(value));
        for(size_t j=0;j<n;++j) CHECK(value[j]==expected);
    }
    return 0;
}
int main(void)
{
    struct medium *m=calloc(1,sizeof(*m)), *baseline=calloc(1,sizeof(*baseline));
    CHECK(m && baseline);
    struct infs20_store_layout l={{1},{2},{3},BLOCKS,{0,32,64}};
    struct infs20_store_io io={m,rd,wr,fl};
    struct infs20_store_state s;
    uint8_t id[16], keys[RECORDS], values[RECORDS][1000];
    struct infs20_store_record records[RECORDS];
    for(unsigned i=0;i<RECORDS;++i) {
        keys[i]=(uint8_t)(i+1); memset(values[i],0x11,sizeof(values[i]));
        records[i]=(struct infs20_store_record){&keys[i],values[i],1,sizeof(values[i])};
    }
    CHECK(infs20_store_recover(&l,&io,&s)==INFS_STATUS_NOT_FOUND);
    txid(id,1);
    CHECK(infs20_store_publish(&l,&io,id,records,RECORDS,&s)==0);
    CHECK(s.sequence==1 && s.leaves>1 && s.fully_protected);
    cold(m);
    CHECK(infs20_store_recover(&l,&io,&s)==0 && s.sequence==1);
    CHECK(verify_values(&l,&io,0x11)==0);
    *baseline=*m;
    for(unsigned i=0;i<RECORDS;++i) memset(values[i],0x22,sizeof(values[i]));
    txid(id,2);
    CHECK(infs20_store_publish(&l,&io,id,records,RECORDS,&s)==0);
    unsigned operations=m->operations;
    CHECK(s.sequence==2 && s.fully_protected && operations>6);
    cold(m);
    CHECK(verify_values(&l,&io,0x22)==0);
    /* Each interruption is applied to the actual writer and cold reader.
     * Simulate refusal, a persisted torn write, and persistence before error.
     * A valid new commit must always imply the complete new tree. */
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) {
        for(unsigned cut=1;cut<=operations;++cut) {
            *m=*baseline; m->mode=mode; m->fail=cut;
            txid(id,100+mode*100+cut);
            memset(&s,0xa5,sizeof(s));
            CHECK(infs20_store_publish(&l,&io,id,records,RECORDS,&s)==INFS_STATUS_IO_ERROR);
            cold(m);
            CHECK(infs20_store_recover(&l,&io,&s)==0);
            CHECK(s.sequence==1 || s.sequence==2);
            CHECK(verify_values(&l,&io,s.sequence==1?0x11:0x22)==0);
            if (!s.fully_protected) {
                txid(id,600);
                CHECK(infs20_store_publish(&l,&io,id,records,RECORDS,&s)==INFS_STATUS_BUSY);
                CHECK(m->operations==0);
                CHECK(infs20_store_heal(&l,&io,&s)==0 && s.fully_protected);
                cold(m);
                CHECK(infs20_store_recover(&l,&io,&s)==0 && s.fully_protected);
            }
            ++cases;
        }
    }
    /* Any two whole lanes may be erased; recover and heal from the third. */
    for(unsigned survivor=0;survivor<3;++survivor) {
        *m=*baseline;
        for(unsigned lane=0;lane<3;++lane) if(lane!=survivor)
            for(unsigned i=0;i<INFS20_STORE_LANE_BLOCKS;++i) erase(m,l.lane_start[lane]+i);
        CHECK(infs20_store_recover(&l,&io,&s)==0 && !s.fully_protected);
        CHECK(verify_values(&l,&io,0x11)==0);
        CHECK(infs20_store_heal(&l,&io,&s)==0 && s.fully_protected);
        cold(m);
        CHECK(infs20_store_recover(&l,&io,&s)==0 && s.fully_protected);
    }
    /* Mixed-copy recovery requires the expected digest at every node. */
    *m=*baseline;
    erase(m,block(&l,0,1,1)); erase(m,block(&l,1,1,1));
    erase(m,block(&l,1,1,2)); erase(m,block(&l,2,1,2));
    CHECK(infs20_store_recover(&l,&io,&s)==0 && !s.fully_protected);
    CHECK(verify_values(&l,&io,0x11)==0);
    CHECK(infs20_store_heal(&l,&io,&s)==0 && s.fully_protected);
    for(unsigned lane=0;lane<3;++lane) erase(m,block(&l,lane,1,2));
    CHECK(infs20_store_recover(&l,&io,&s)==INFS_STATUS_CORRUPT);
    CHECK(infs20_store_heal(&l,&io,&s)==INFS_STATUS_CORRUPT);
    /* Duplicate/unordered keys, overlap and capacity failures write nothing. */
    *m=*baseline; txid(id,7); keys[1]=keys[0];
    CHECK(infs20_store_publish(&l,&io,id,records,RECORDS,&s)==INFS_STATUS_INVALID_ARGUMENT);
    keys[1]=2;
    struct infs20_store_layout bad=l; bad.lane_start[1]=1;
    CHECK(infs20_store_publish(&bad,&io,id,records,RECORDS,&s)==INFS_STATUS_INVALID_ARGUMENT);
    records[0].value_size=4000;
    CHECK(infs20_store_publish(&l,&io,id,records,RECORDS,&s)==INFS_STATUS_NO_SPACE);
    records[0].value_size=1000;
    CHECK(m->operations==0);
    CHECK(!memcmp(m->durable,baseline->durable,sizeof(m->durable)));
    /* Empty tree is a real committed root, not absence of a filesystem. */
    CHECK(infs20_store_publish(&l,&io,id,NULL,0,&s)==0 && s.records==0);
    cold(m);
    CHECK(infs20_store_recover(&l,&io,&s)==0 && s.sequence==2 && s.fully_protected);
    size_t n=99; uint8_t value=0;
    CHECK(infs20_store_lookup(&l,&io,keys,1,&value,1,&n)==INFS_STATUS_NOT_FOUND && n==99);
    printf("format20-store: PASS (%u actual-writer interruption cases; lane/mixed-copy recovery)\n",cases);
    free(baseline); free(m);
    return 0;
}
