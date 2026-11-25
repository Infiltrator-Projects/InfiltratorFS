// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-reclaim:%d: %s\n",__LINE__,#x); return 1; } } while(0)
/* Exactly two payload gaps. Snapshot, fallback, and reuse are exercised
 * through the real experimental writer, not a parallel allocator model. */
enum { LANE=640, BLOCKS=3*LANE, UNITS=16, BYTES=UNITS*INFS20_PAGE_BYTES };
static const size_t disk_bytes=(size_t)BLOCKS*INFS20_PAGE_BYTES;
struct medium { uint8_t *disk,*pending; unsigned ops,fail,mode; };
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
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
        if(m->mode==2) memcpy(m->disk,m->pending,disk_bytes);
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,disk_bytes); return 0;
}
static void cold(struct medium *m)
{
    memcpy(m->pending,m->disk,disk_bytes); m->ops=m->fail=0;
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[15]=0x94;
}
static int same(const struct infs20_paged_layout *l,const struct infs20_store_io *io,
    uint64_t pin,const char *name,const uint8_t *data)
{
    uint8_t out[BYTES]; size_t n=123;
    infs_status st=infs20_paged_volume_read(l,io,pin,name,out,sizeof(out),&n);
    return data?(!st && n==BYTES && !memcmp(out,data,n)):
                (st==INFS_STATUS_NOT_FOUND && n==123);
}
int main(void)
{
    struct medium m={0};
    m.disk=calloc(1,disk_bytes); m.pending=calloc(1,disk_bytes);
    uint8_t *base=malloc(disk_bytes),*a=malloc(BYTES),*b=malloc(BYTES);
    CHECK(m.disk && m.pending && base && a && b);
    memset(a,0x39,BYTES); memset(b,0xa7,BYTES);
    struct infs20_paged_layout l={{{1},{2},{3},BLOCKS,{0,LANE,2*LANE}},
       {20,LANE+20,2*LANE+20},{LANE-36,LANE-36,LANE-20}};
    struct infs20_store_io io={&m,rd,wr,fl};
    uint8_t id[16]; tx(id,1);
    CHECK(!infs20_paged_volume_create(&l,&io,id)); cold(&m);
    tx(id,2); CHECK(!infs20_paged_volume_write(&l,&io,id,"a",a,BYTES)); cold(&m);
    tx(id,3); CHECK(!infs20_paged_volume_snapshot(&l,&io,id,10)); cold(&m);
    tx(id,4); CHECK(!infs20_paged_volume_write(&l,&io,id,"b",b,BYTES)); cold(&m);
    tx(id,5); CHECK(!infs20_paged_volume_unlink(&l,&io,id,"a")); cold(&m);
    CHECK(same(&l,&io,0,"a",NULL) && same(&l,&io,10,"a",a) &&
          same(&l,&io,0,"b",b));
    tx(id,6); CHECK(!infs20_paged_volume_collect(&l,&io,id)); cold(&m);
    tx(id,7); CHECK(infs20_paged_volume_write(&l,&io,id,"c",a,BYTES)==INFS_STATUS_NO_SPACE);
    CHECK(!m.ops && same(&l,&io,10,"a",a));
    tx(id,8); CHECK(!infs20_paged_volume_release_pin(&l,&io,id,10)); cold(&m);
    tx(id,9); CHECK(!infs20_paged_volume_collect(&l,&io,id)); cold(&m);
    CHECK(!memcmp(m.disk+(size_t)(LANE-16)*INFS20_PAGE_BYTES,a,BYTES));
    tx(id,10); CHECK(infs20_paged_volume_write(&l,&io,id,"c",b,BYTES)==INFS_STATUS_NO_SPACE);
    CHECK(!m.ops && same(&l,&io,0,"b",b));
    tx(id,11); CHECK(!infs20_paged_volume_collect(&l,&io,id)); cold(&m);
    memcpy(base,m.disk,disk_bytes);
    tx(id,12); CHECK(!infs20_paged_volume_write(&l,&io,id,"c",b,BYTES));
    unsigned cuts=m.ops; cold(&m);
    CHECK(same(&l,&io,0,"b",b) && same(&l,&io,0,"c",b));
    CHECK(!memcmp(m.disk+(size_t)(LANE-16)*INFS20_PAGE_BYTES,b,BYTES));
    struct infs20_paged_state committed;
    CHECK(!infs20_paged_open(&l,&io,&committed));
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=cuts;++cut) {
        memcpy(m.disk,base,disk_bytes); cold(&m); m.mode=mode; m.fail=cut;
        tx(id,100+mode*200+cut);
        CHECK(infs20_paged_volume_write(&l,&io,id,"c",b,BYTES)==INFS_STATUS_IO_ERROR);
        cold(&m);
        struct infs20_paged_state state;
        CHECK(!infs20_paged_open(&l,&io,&state));
        CHECK(state.commit.sequence==committed.commit.sequence ||
              state.commit.sequence+1==committed.commit.sequence);
        CHECK(same(&l,&io,0,"b",b));
        CHECK(same(&l,&io,0,"c",state.commit.sequence==committed.commit.sequence?b:NULL));
        ++cases;
    }
    free(b); free(a); free(base); free(m.pending); free(m.disk);
    printf("format20-reclaim: PASS (%u physical reuse interruption cases; snapshot and fallback quarantine)\n",cases);
    return 0;
}
