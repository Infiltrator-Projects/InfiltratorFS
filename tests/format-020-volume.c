// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-volume:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { BLOCKS=96 };
struct medium {
    uint8_t disk[BLOCKS][INFS20_PAGE_BYTES], pending[BLOCKS][INFS20_PAGE_BYTES];
    unsigned ops,fail,mode,payload_writes;
};
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o; if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending[b],INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o; if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    if(b%32>=INFS20_STORE_LANE_BLOCKS) ++m->payload_writes;
    if(++m->ops==m->fail) {
        if(m->mode) {
            size_t n=m->mode==1?INFS20_PAGE_BYTES/2:INFS20_PAGE_BYTES;
            memcpy(m->disk[b],p,n); memcpy(m->pending[b],p,n);
        }
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->pending[b],p,INFS20_PAGE_BYTES); return 0;
}
static infs_status fl(void *o)
{
    struct medium *m=o;
    if(++m->ops==m->fail) {
        if(m->mode==2) memcpy(m->disk,m->pending,sizeof(m->disk));
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,sizeof(m->disk)); return 0;
}
static void cold(struct medium *m)
{
    memcpy(m->pending,m->disk,sizeof(m->disk)); m->ops=0; m->fail=0;
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[2]=0xb5;
}
static int expect(const struct infs20_store_layout *l,const struct infs20_store_io *io,
    uint64_t pin,const char *name,const char *value)
{
    uint8_t b[32]; size_t n=99;
    memset(b,0x5a,sizeof(b));
    infs_status st=infs20_volume_read(l,io,pin,name,b,sizeof(b),&n);
    if(!value) { CHECK(st==INFS_STATUS_NOT_FOUND && n==99 && b[0]==0x5a); }
    else { CHECK(st==0 && n==strlen(value) && !memcmp(b,value,n)); }
    return 0;
}
int main(void)
{
    struct medium *m=calloc(1,sizeof(*m)),*base=calloc(1,sizeof(*base));
    CHECK(m && base);
    struct infs20_store_layout l={{1},{2},{3},BLOCKS,{0,32,64}};
    struct infs20_store_io io={m,rd,wr,fl};
    uint8_t id[16];
    tx(id,1); CHECK(infs20_volume_create(&l,&io,id)==0); cold(m);
    CHECK(expect(&l,&io,0,"a",NULL)==0);
    tx(id,2); CHECK(infs20_volume_write(&l,&io,id,"a",(const uint8_t *)"original",8)==0);
    cold(m); CHECK(expect(&l,&io,0,"a","original")==0);
    unsigned payload=m->payload_writes;
    tx(id,3); CHECK(infs20_volume_clone(&l,&io,id,"a","b")==0);
    CHECK(m->payload_writes==payload); cold(m);
    CHECK(expect(&l,&io,0,"b","original")==0);
    tx(id,4); CHECK(infs20_volume_snapshot(&l,&io,id,10)==0); cold(m);
    *base=*m;
    tx(id,5); CHECK(infs20_volume_write(&l,&io,id,"a",(const uint8_t *)"changed",7)==0);
    unsigned ops=m->ops; cold(m);
    CHECK(expect(&l,&io,0,"a","changed")==0 && expect(&l,&io,0,"b","original")==0);
    CHECK(expect(&l,&io,10,"a","original")==0);
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=ops;++cut) {
        *m=*base; m->mode=mode; m->fail=cut; tx(id,100+mode*100+cut);
        CHECK(infs20_volume_write(&l,&io,id,"a",(const uint8_t *)"changed",7)==INFS_STATUS_IO_ERROR);
        cold(m);
        struct infs20_store_state s;
        CHECK(infs20_store_recover(&l,&io,&s)==0);
        CHECK(s.sequence==4 || s.sequence==5);
        CHECK(expect(&l,&io,0,"a",s.sequence==4?"original":"changed")==0);
        CHECK(expect(&l,&io,0,"b","original")==0 && expect(&l,&io,10,"a","original")==0);
        ++cases;
    }
    *m=*base; tx(id,6);
    CHECK(infs20_volume_write(&l,&io,id,"a",(const uint8_t *)"changed",7)==0); cold(m);
    payload=m->payload_writes;
    tx(id,7); CHECK(infs20_volume_relocate(&l,&io,id,"b")==0); cold(m);
    CHECK(m->payload_writes==payload+1);
    CHECK(expect(&l,&io,0,"b","original")==0 && expect(&l,&io,10,"a","original")==0);
    /* The original location was 20, second allocation 21, relocation 22.
     * Damage the old location; both shared mappings must resolve the new one. */
    memset(m->disk[20],0,INFS20_PAGE_BYTES); cold(m);
    CHECK(expect(&l,&io,0,"b","original")==0 && expect(&l,&io,10,"a","original")==0);
    payload=m->payload_writes;
    tx(id,8); CHECK(infs20_volume_format(&l,&io,id,11)==0); cold(m);
    CHECK(m->payload_writes==payload);
    CHECK(expect(&l,&io,0,"a",NULL)==0 && expect(&l,&io,11,"a","changed")==0);
    tx(id,9); CHECK(infs20_volume_write(&l,&io,id,"new",(const uint8_t *)"new data",8)==0); cold(m);
    tx(id,10); CHECK(infs20_volume_unformat(&l,&io,id,11)==0); cold(m);
    CHECK(expect(&l,&io,0,"a","changed")==0 && expect(&l,&io,0,"b","original")==0);
    CHECK(expect(&l,&io,11,"new","new data")==0 && expect(&l,&io,0,"new",NULL)==0);
    /* Two complete metadata lanes destroyed: third proves committed namespace
     * and location mappings. Read-only recovery does not write to source. */
    memset(m->disk[0],0,INFS20_STORE_LANE_BLOCKS*INFS20_PAGE_BYTES);
    memset(m->disk[32],0,INFS20_STORE_LANE_BLOCKS*INFS20_PAGE_BYTES); cold(m);
    CHECK(expect(&l,&io,0,"a","changed")==0 && expect(&l,&io,11,"new","new data")==0);
    CHECK(m->ops==0);
    memset(m->disk[22],0,INFS20_PAGE_BYTES); cold(m);
    uint8_t out[16]; memset(out,0x5a,sizeof(out)); size_t n=99;
    CHECK(infs20_volume_read(&l,&io,0,"b",out,sizeof(out),&n)==INFS_STATUS_CORRUPT);
    CHECK(n==99 && out[0]==0x5a && m->ops==0);
    printf("format20-volume: PASS (%u payload/metadata interruption cases; shared relocation and native undo)\n",cases);
    free(base); free(m); return 0;
}
