// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-multiblock:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { BLOCKS=192, SIZE=2*INFS20_PAGE_BYTES+37 };
struct medium {
    uint8_t disk[BLOCKS][INFS20_PAGE_BYTES],pending[BLOCKS][INFS20_PAGE_BYTES];
    unsigned operations,fail,mode,writes;
    uint64_t unreadable;
};
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o;
    if(b>=BLOCKS || b==m->unreadable) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending[b],INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    ++m->writes;
    if(++m->operations==m->fail) {
        if(m->mode==3) return INFS_STATUS_OK;
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
    if(++m->operations==m->fail) {
        if(m->mode==2) memcpy(m->disk,m->pending,sizeof(m->disk));
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->disk,m->pending,sizeof(m->disk)); return 0;
}
static void cold(struct medium *m)
{
    memcpy(m->pending,m->disk,sizeof(m->disk));
    m->operations=m->fail=0;
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[15]=0xa9;
}
static int same(const struct infs20_store_layout *l,const struct infs20_store_io *io,
    uint64_t pin,const char *name,const uint8_t *expected,size_t size)
{
    uint8_t out[INFS20_VOLUME_BYTES]; size_t n=0;
    return infs20_volume_read(l,io,pin,name,out,sizeof(out),&n)==0 &&
           n==size && !memcmp(out,expected,size);
}
struct scan {
    uint8_t allocation[16]; unsigned maps,units,valid,missing,unreadable;
    unsigned distinct; uint64_t first_block;
    const uint8_t *expected;
};
static infs_status mapping(void *o,const struct infs20_recovery_mapping *m)
{
    struct scan *s=o;
    if(m->size!=SIZE || (strcmp(m->name,"original") && strcmp(m->name,"clone")))
        return INFS_STATUS_CORRUPT;
    if(!s->maps) memcpy(s->allocation,m->allocation_id,16);
    if(memcmp(s->allocation,m->allocation_id,16)) return INFS_STATUS_CORRUPT;
    if(m->pin!=0 && m->pin!=10) return INFS_STATUS_CORRUPT;
    ++s->maps; return 0;
}
static infs_status unit(void *o,const struct infs20_recovery_unit *u)
{
    struct scan *s=o;
    if(memcmp(s->allocation,u->allocation_id,16) ||
       u->logical_offset!=s->units*INFS20_PAGE_BYTES ||
       u->logical_size!=(s->units==2?37:INFS20_PAGE_BYTES))
        return INFS_STATUS_CORRUPT;
    if(!s->units) s->first_block=u->physical_block;
    if(u->physical_block!=s->first_block+s->units) return INFS_STATUS_CORRUPT;
    ++s->units;
    if(u->status==INFS_STATUS_OK) {
        if(!u->data || memcmp(u->data,s->expected+u->logical_offset,u->logical_size))
            return INFS_STATUS_CORRUPT;
        ++s->valid;
    } else {
        if(u->data) return INFS_STATUS_CORRUPT;
        if(u->status==INFS_STATUS_CORRUPT) ++s->missing;
        else if(u->status==INFS_STATUS_IO_ERROR) ++s->unreadable;
        else return u->status;
    }
    return 0;
}
int main(void)
{
    struct medium *m=calloc(1,sizeof(*m)),*base=calloc(1,sizeof(*base));
    CHECK(m && base); m->unreadable=UINT64_MAX;
    struct infs20_store_layout l={{1},{2},{3},BLOCKS,{0,64,128}};
    struct infs20_store_io io={m,rd,wr,fl};
    uint8_t id[16],original[SIZE],replacement[SIZE],out[SIZE];
    for(size_t i=0;i<SIZE;++i) { original[i]=(uint8_t)(i*17+3); replacement[i]=(uint8_t)(i*23+9); }
    tx(id,1); CHECK(!infs20_volume_create(&l,&io,id)); cold(m);
    tx(id,2); CHECK(!infs20_volume_write(&l,&io,id,"original",original,SIZE)); cold(m);
    CHECK(same(&l,&io,0,"original",original,SIZE));
    tx(id,3); CHECK(!infs20_volume_clone(&l,&io,id,"original","clone")); cold(m);
    tx(id,4); CHECK(!infs20_volume_snapshot(&l,&io,id,10)); cold(m);
    *base=*m;
    tx(id,5); CHECK(!infs20_volume_write(&l,&io,id,"original",replacement,SIZE));
    unsigned ops=m->operations; cold(m);
    CHECK(same(&l,&io,0,"original",replacement,SIZE));
    CHECK(same(&l,&io,0,"clone",original,SIZE) && same(&l,&io,10,"original",original,SIZE));
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=ops;++cut) {
        *m=*base; m->mode=mode; m->fail=cut; tx(id,100+mode*100+cut);
        CHECK(infs20_volume_write(&l,&io,id,"original",replacement,SIZE)==INFS_STATUS_IO_ERROR);
        cold(m);
        struct infs20_store_state st; CHECK(!infs20_store_recover(&l,&io,&st));
        CHECK(st.sequence==4 || st.sequence==5);
        CHECK(same(&l,&io,0,"original",st.sequence==4?original:replacement,SIZE));
        CHECK(same(&l,&io,0,"clone",original,SIZE) && same(&l,&io,10,"original",original,SIZE));
        ++cases;
    }
    /* Relocation changes only physical ownership. Four pinned mappings still
     * identify one allocation and its three digest units. */
    *m=*base; tx(id,6); CHECK(!infs20_volume_relocate(&l,&io,id,"clone"));
    ops=m->operations; cold(m);
    struct scan s={0}; s.expected=original;
    CHECK(!infs20_volume_recovery_visit(&l,&io,mapping,unit,&s));
    CHECK(s.maps==4 && s.units==3 && s.valid==3 && s.first_block==23);
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=ops;++cut) {
        *m=*base; m->mode=mode; m->fail=cut; tx(id,500+mode*100+cut);
        CHECK(infs20_volume_relocate(&l,&io,id,"clone")==INFS_STATUS_IO_ERROR);
        cold(m);
        struct infs20_store_state st; CHECK(!infs20_store_recover(&l,&io,&st));
        CHECK(st.sequence==4 || st.sequence==5);
        CHECK(same(&l,&io,0,"original",original,SIZE) && same(&l,&io,10,"clone",original,SIZE));
        ++cases;
    }
    /* The same dependency admission applies to physical relocation. */
    *m=*base; m->mode=3; m->fail=2; tx(id,799);
    CHECK(infs20_volume_relocate(&l,&io,id,"clone")==INFS_STATUS_CORRUPT);
    cold(m); struct infs20_store_state dropped;
    CHECK(!infs20_store_recover(&l,&io,&dropped) && dropped.sequence==4);
    CHECK(same(&l,&io,0,"original",original,SIZE) && same(&l,&io,10,"clone",original,SIZE));
    *m=*base; tx(id,7); CHECK(!infs20_volume_relocate(&l,&io,id,"clone")); cold(m);
    memset(m->disk[20],0,3*INFS20_PAGE_BYTES); cold(m);
    CHECK(same(&l,&io,0,"original",original,SIZE) && same(&l,&io,10,"clone",original,SIZE));
    tx(id,8); CHECK(!infs20_volume_format(&l,&io,id,11)); cold(m);
    tx(id,9); CHECK(!infs20_volume_write(&l,&io,id,"later",replacement,SIZE)); cold(m);
    tx(id,10); CHECK(!infs20_volume_unformat(&l,&io,id,11)); cold(m);
    CHECK(same(&l,&io,0,"original",original,SIZE) && same(&l,&io,11,"later",replacement,SIZE));
    /* Use the pre-format model for exact loss enumeration. Destroy two lanes
     * and one payload unit; another unit has a simulated read error. */
    *m=*base; tx(id,11); CHECK(!infs20_volume_relocate(&l,&io,id,"clone")); cold(m);
    memset(m->disk[0],0,INFS20_STORE_LANE_BLOCKS*INFS20_PAGE_BYTES);
    memset(m->disk[64],0,INFS20_STORE_LANE_BLOCKS*INFS20_PAGE_BYTES);
    m->disk[24][123]^=1; cold(m); m->unreadable=25;
    unsigned writes=m->writes;
    memset(&s,0,sizeof(s)); s.expected=original;
    CHECK(!infs20_volume_recovery_visit(&l,&io,mapping,unit,&s));
    CHECK(s.maps==4 && s.units==3 && s.valid==1 && s.missing==1 && s.unreadable==1);
    CHECK(m->writes==writes && m->operations==0);
    size_t n=99; memset(out,0xa5,sizeof(out));
    CHECK(infs20_volume_read(&l,&io,0,"original",out,sizeof(out),&n)==INFS_STATUS_CORRUPT);
    CHECK(n==99 && out[0]==0xa5 && out[SIZE-1]==0xa5);
    CHECK(!infs20_volume_read_range(&l,&io,0,"clone",13,out,4096-13,&n));
    CHECK(n==4096-13 && !memcmp(out,original+13,n));
    memset(out,0xa5,sizeof(out)); n=99;
    CHECK(infs20_volume_read_range(&l,&io,0,"original",4095,out,3,&n)==INFS_STATUS_CORRUPT);
    CHECK(n==99 && out[0]==0xa5 && out[2]==0xa5);
    CHECK(!infs20_volume_read_range(&l,&io,0,"original",UINT64_MAX,out,3,&n) && !n);
    CHECK(m->writes==writes);

    /* Largest allowed contiguous allocation must skip an intersected lane.
     * Fill frontier 20..61 using three allocations, then place sixteen units
     * beyond the complete lane at [64,84). */
    memset(m,0,sizeof(*m)); m->unreadable=UINT64_MAX;
    uint8_t *large=malloc(INFS20_VOLUME_BYTES); CHECK(large);
    memset(large,0x5c,INFS20_VOLUME_BYTES);
    tx(id,20); CHECK(!infs20_volume_create(&l,&io,id)); cold(m);
    tx(id,21); CHECK(!infs20_volume_write(&l,&io,id,"large",large,INFS20_VOLUME_BYTES)); cold(m);
    tx(id,22); CHECK(!infs20_volume_write(&l,&io,id,"other",large,INFS20_VOLUME_BYTES)); cold(m);
    tx(id,23); CHECK(!infs20_volume_write(&l,&io,id,"ten",large,10*INFS20_PAGE_BYTES)); cold(m);
    tx(id,24); CHECK(!infs20_volume_write(&l,&io,id,"skip",large,INFS20_VOLUME_BYTES)); cold(m);
    CHECK(same(&l,&io,0,"skip",large,INFS20_VOLUME_BYTES));
    CHECK(!memcmp(m->disk[84],large,INFS20_PAGE_BYTES));
    writes=m->writes; tx(id,25);
    CHECK(infs20_volume_write(&l,&io,id,"oversize",large,INFS20_VOLUME_BYTES+1)==INFS_STATUS_FILE_TOO_LARGE);
    CHECK(m->writes==writes);
    free(large); free(base); free(m);
    printf("format20-multiblock: PASS (%u write/relocation interruption cases; indexed integrity and partial recovery)\n",cases);
    return 0;
}
