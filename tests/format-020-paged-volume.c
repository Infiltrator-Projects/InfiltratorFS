// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-paged-volume:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { LANE=768, BLOCKS=3*LANE, LENGTH=2*INFS20_PAGE_BYTES+73, DATA=620 };
static const size_t bytes=(size_t)BLOCKS*INFS20_PAGE_BYTES;
struct medium { uint8_t *disk,*pending; unsigned ops,fail,mode; };
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o; if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->pending+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o; if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    if(++m->ops==m->fail) {
        if(m->mode==3) return INFS_STATUS_OK;
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
    memset(id,0,16); id[0]=(uint8_t)n; id[1]=(uint8_t)(n>>8); id[15]=0x31;
}
static int same(const struct infs20_paged_layout *l,const struct infs20_store_io *io,
    uint64_t pin,const char *name,const uint8_t *expected)
{
    uint8_t out[LENGTH]; size_t n=99; memset(out,0xa5,sizeof(out));
    infs_status st=infs20_paged_volume_read(l,io,pin,name,out,sizeof(out),&n);
    return expected?(!st && n==LENGTH && !memcmp(out,expected,n)):
                    (st==INFS_STATUS_NOT_FOUND && n==99 && out[0]==0xa5);
}
struct raw { uint8_t *p; size_t n; };
static infs_status raw_read(void *o,uint64_t off,uint8_t *p,size_t n)
{
    struct raw *r=o;
    if(off>r->n || n>r->n-(size_t)off) return INFS_STATUS_IO_ERROR;
    memcpy(p,r->p+(size_t)off,n); return 0;
}
struct scan { unsigned maps,units,bad; uint64_t bad_block; };
static infs_status mapping(void *o,const struct infs20_recovery_mapping *m)
{
    struct scan *s=o;
    if(m->size!=LENGTH || !m->name) return INFS_STATUS_CORRUPT;
    ++s->maps; return 0;
}
static infs_status unit(void *o,const struct infs20_recovery_unit *u)
{
    struct scan *s=o; ++s->units;
    if(u->status) {
        if(u->status!=INFS_STATUS_CORRUPT || u->data) return INFS_STATUS_CORRUPT;
        ++s->bad; s->bad_block=u->physical_block;
    } else if(!u->data) return INFS_STATUS_CORRUPT;
    return 0;
}
struct names {
    const char *expected[4];
    size_t seen;
    uint8_t first_id[16];
    int shared;
};
static infs_status listed(void *opaque,const struct infs20_namespace_entry *e)
{
    struct names *n=opaque;
    if(n->seen>=4 || !n->expected[n->seen] ||
       strcmp(e->name,n->expected[n->seen]) || e->size!=LENGTH)
        return INFS_STATUS_CORRUPT;
    if(!n->seen) memcpy(n->first_id,e->allocation_id,16);
    else if(n->shared && memcmp(n->first_id,e->allocation_id,16))
        return INFS_STATUS_CORRUPT;
    ++n->seen; return INFS_STATUS_OK;
}
struct scope_check {
    uint8_t id[16];
    size_t count;
    int initialised;
};
static infs_status scope_mapping(void *opaque,const struct infs20_recovery_mapping *m)
{
    struct scope_check *s=opaque;
    if(m->pin) return INFS_STATUS_OK;
    if(!s->initialised) { memcpy(s->id,m->root_id,16); s->initialised=1; }
    if(memcmp(s->id,m->root_id,16)) return INFS_STATUS_CORRUPT;
    ++s->count; return INFS_STATUS_OK;
}
static infs_status scope_unit(void *opaque,const struct infs20_recovery_unit *u)
{
    (void)opaque;
    return u->status;
}
int main(void)
{
    struct medium m={0};
    m.disk=calloc(1,bytes); m.pending=calloc(1,bytes);
    uint8_t *baseline=malloc(bytes);
    CHECK(m.disk && m.pending && baseline);
    struct infs20_paged_layout l={{{1},{2},{3},BLOCKS,{0,LANE,2*LANE}},
        {20,LANE+20,2*LANE+20},{600,600,600}};
    struct infs20_store_io io={&m,rd,wr,fl};
    uint8_t id[16],a[LENGTH],b[LENGTH];
    for(size_t i=0;i<LENGTH;++i) { a[i]=(uint8_t)(i*7+3); b[i]=(uint8_t)(i*19+9); }
    tx(id,1);
    CHECK(infs20_paged_volume_create_at(&l,&io,id,BLOCKS)==INFS_STATUS_INVALID_ARGUMENT);
    CHECK(!m.ops);
    CHECK(!infs20_paged_volume_create(&l,&io,id)); cold(&m);
    tx(id,2); CHECK(!infs20_paged_volume_write(&l,&io,id,"a",a,LENGTH)); cold(&m);
    CHECK(!memcmp(m.disk+(size_t)DATA*INFS20_PAGE_BYTES,a,INFS20_PAGE_BYTES));
    tx(id,3); CHECK(!infs20_paged_volume_clone(&l,&io,id,"a","clone")); cold(&m);
    tx(id,4); CHECK(!infs20_paged_volume_snapshot(&l,&io,id,10)); cold(&m);
    struct names names={{"a","clone",NULL,NULL},0,{0},1};
    size_t listed_count=99; int more=99; uint64_t listed_sequence=0;
    CHECK(!infs20_paged_volume_list(&l,&io,10,0,NULL,1,listed,&names,
        &listed_count,&more,&listed_sequence));
    CHECK(names.seen==1 && listed_count==1 && more && listed_sequence==4);
    CHECK(!infs20_paged_volume_list(&l,&io,10,listed_sequence,"a",1,listed,&names,
        &listed_count,&more,&listed_sequence));
    CHECK(names.seen==2 && listed_count==1 && !more && listed_sequence==4);
    CHECK(!infs20_paged_volume_list(&l,&io,10,listed_sequence,"clone",1,listed,&names,
        &listed_count,&more,&listed_sequence));
    CHECK(names.seen==2 && !listed_count && !more);
    names.seen=0; listed_count=99; more=99;
    CHECK(infs20_paged_volume_list(&l,&io,10,3,NULL,1,listed,&names,
        &listed_count,&more,&listed_sequence)==INFS_STATUS_BUSY);
    CHECK(!names.seen && listed_count==99 && more==99 && listed_sequence==4);
    CHECK(infs20_paged_volume_list(&l,&io,10,0,"a",1,listed,&names,
        &listed_count,&more,&listed_sequence)==INFS_STATUS_INVALID_ARGUMENT);
    uint8_t range[53]; size_t got=99;
    CHECK(!infs20_paged_volume_read_range(&l,&io,10,"a",
        INFS20_PAGE_BYTES-17,range,sizeof(range),&got));
    CHECK(got==sizeof(range) && !memcmp(range,a+INFS20_PAGE_BYTES-17,got));
    CHECK(!infs20_paged_volume_read_range(&l,&io,10,"a",UINT64_MAX,
        range,sizeof(range),&got) && !got);
    memset(range,0xa5,sizeof(range)); got=99;
    CHECK(infs20_paged_volume_read_range(&l,&io,9999,"a",0,
        range,sizeof(range),&got)==INFS_STATUS_NOT_FOUND && got==99 && range[0]==0xa5);
    /* A corrupt touched unit refuses without publishing even a verified
     * preceding fragment. An untouched unit is not read by a range query. */
    m.pending[(size_t)(DATA+1)*INFS20_PAGE_BYTES+10]^=1;
    CHECK(!infs20_paged_volume_read_range(&l,&io,0,"a",0,
        range,sizeof(range),&got) && got==sizeof(range) && !memcmp(range,a,got));
    memset(range,0xa5,sizeof(range)); got=99;
    CHECK(infs20_paged_volume_read_range(&l,&io,0,"a",INFS20_PAGE_BYTES-17,
        range,sizeof(range),&got)==INFS_STATUS_CORRUPT && got==99 && range[0]==0xa5);
    cold(&m);
    memcpy(baseline,m.disk,bytes);
    tx(id,5); CHECK(!infs20_paged_volume_write(&l,&io,id,"a",b,LENGTH));
    unsigned cuts=m.ops; cold(&m);
    CHECK(same(&l,&io,0,"a",b) && same(&l,&io,0,"clone",a) && same(&l,&io,10,"a",a));
    unsigned cases=0;
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=cuts;++cut) {
        memcpy(m.disk,baseline,bytes); cold(&m); m.mode=mode; m.fail=cut;
        tx(id,100+mode*200+cut);
        CHECK(infs20_paged_volume_write(&l,&io,id,"a",b,LENGTH)==INFS_STATUS_IO_ERROR);
        cold(&m); struct infs20_paged_state s;
        CHECK(!infs20_paged_open(&l,&io,&s));
        CHECK(s.commit.sequence==4 || s.commit.sequence==5);
        CHECK(same(&l,&io,0,"a",s.commit.sequence==4?a:b));
        CHECK(same(&l,&io,0,"clone",a) && same(&l,&io,10,"a",a));
        ++cases;
    }
    memcpy(m.disk,baseline,bytes); cold(&m);
    /* The device acknowledges a dropped middle payload write. Neither
     * checksum-valid metadata nor a successful flush can authorize it. */
    m.mode=3; m.fail=2; tx(id,799);
    CHECK(infs20_paged_volume_write(&l,&io,id,"a",b,LENGTH)==INFS_STATUS_CORRUPT);
    cold(&m); struct infs20_paged_state dropped;
    CHECK(!infs20_paged_open(&l,&io,&dropped) && dropped.commit.sequence==4);
    CHECK(same(&l,&io,0,"a",a) && same(&l,&io,0,"clone",a) && same(&l,&io,10,"a",a));
    memcpy(m.disk,baseline,bytes); cold(&m);
    tx(id,800); CHECK(!infs20_paged_volume_write(&l,&io,id,"a",b,LENGTH)); cold(&m);
    tx(id,801); CHECK(!infs20_paged_volume_relocate(&l,&io,id,"clone")); cold(&m);
    memset(m.disk+(size_t)DATA*INFS20_PAGE_BYTES,0,3*INFS20_PAGE_BYTES); cold(&m);
    CHECK(same(&l,&io,0,"clone",a) && same(&l,&io,10,"a",a));
    tx(id,802); CHECK(!infs20_paged_volume_format(&l,&io,id,11)); cold(&m);
    CHECK(same(&l,&io,0,"a",NULL) && same(&l,&io,11,"a",b));
    tx(id,803); CHECK(!infs20_paged_volume_write(&l,&io,id,"new",a,LENGTH)); cold(&m);
    tx(id,804); CHECK(!infs20_paged_volume_unformat(&l,&io,id,11)); cold(&m);
    CHECK(same(&l,&io,0,"a",b) && same(&l,&io,0,"clone",a) && same(&l,&io,11,"new",a));
    /* Erase bootstrap and arena ranges of two lanes, not payload gaps. */
    for(unsigned lane=0;lane<2;++lane)
        memset(m.disk+(size_t)l.bootstrap.lane_start[lane]*INFS20_PAGE_BYTES,0,
               (20+600)*INFS20_PAGE_BYTES);
    cold(&m);
    CHECK(same(&l,&io,0,"a",b) && same(&l,&io,10,"a",a));
    struct raw raw={calloc(1,bytes+512),bytes+512}; CHECK(raw.p);
    memcpy(raw.p+512,m.disk,bytes);
    struct infs20_raw_source source={&raw,raw.n,raw_read};
    struct infs20_store_region region; struct infs20_paged_layout found;
    struct infs20_paged_state state;
    CHECK(!infs20_paged_discover(&source,&region,&found,&state) && region.start==512);
    struct infs20_store_io ro=infs20_store_region_io(&region);
    CHECK(!ro.write && !ro.flush && same(&found,&ro,0,"clone",a));
    /* Shared relocated allocation starts after original and replacement.
     * Corrupt one unit; exact missing range is emitted once, not per pin. */
    raw.p[512+(size_t)(DATA+7)*INFS20_PAGE_BYTES+9]^=1;
    struct scan scan={0};
    CHECK(!infs20_paged_volume_recovery_visit(&found,&ro,mapping,unit,&scan));
    CHECK(scan.maps==5 && scan.units==9 && scan.bad==1 && scan.bad_block==DATA+7);
    CHECK(same(&found,&ro,0,"a",b) && same(&found,&ro,11,"new",a));
    /* Atomic rename uses the same existing namespace and ownership model.
     * Reinitialise the disposable image to isolate this fault matrix. */
    memset(m.disk,0,bytes); cold(&m);
    tx(id,900); CHECK(!infs20_paged_volume_create(&l,&io,id)); cold(&m);
    tx(id,901); CHECK(!infs20_paged_volume_write(&l,&io,id,"src",a,LENGTH)); cold(&m);
    tx(id,902); CHECK(!infs20_paged_volume_write(&l,&io,id,"dst",b,LENGTH)); cold(&m);
    tx(id,903); CHECK(!infs20_paged_volume_clone(&l,&io,id,"src","alias")); cold(&m);
    tx(id,904); CHECK(!infs20_paged_volume_snapshot(&l,&io,id,20)); cold(&m);
    tx(id,905);
    CHECK(infs20_paged_volume_rename(&l,&io,id,"src","dst",0)==INFS_STATUS_ALREADY_EXISTS);
    CHECK(!m.ops);
    CHECK(infs20_paged_volume_rename(&l,&io,id,"absent","dst",1)==INFS_STATUS_NOT_FOUND);
    CHECK(!m.ops);
    CHECK(!infs20_paged_volume_rename(&l,&io,id,"src","src",0) && !m.ops);
    memcpy(baseline,m.disk,bytes);
    tx(id,906); CHECK(!infs20_paged_volume_rename(&l,&io,id,"src","dst",1));
    unsigned rename_cuts=m.ops; cold(&m);
    CHECK(same(&l,&io,0,"src",NULL) && same(&l,&io,0,"dst",a));
    CHECK(same(&l,&io,0,"alias",a) && same(&l,&io,20,"src",a) &&
          same(&l,&io,20,"dst",b));
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=rename_cuts;++cut) {
        memcpy(m.disk,baseline,bytes); cold(&m); m.mode=mode; m.fail=cut;
        tx(id,1000+mode*300+cut);
        CHECK(infs20_paged_volume_rename(&l,&io,id,"src","dst",1)==INFS_STATUS_IO_ERROR);
        cold(&m); struct infs20_paged_state current;
        CHECK(!infs20_paged_open(&l,&io,&current));
        CHECK(current.commit.sequence==5 || current.commit.sequence==6);
        int changed=current.commit.sequence==6;
        CHECK(same(&l,&io,0,"src",changed?NULL:a) && same(&l,&io,0,"dst",changed?a:b));
        CHECK(same(&l,&io,0,"alias",a) && same(&l,&io,20,"src",a) &&
              same(&l,&io,20,"dst",b));
        ++cases;
    }
    memcpy(m.disk,baseline,bytes); cold(&m);
    tx(id,2000); CHECK(!infs20_paged_volume_rename(&l,&io,id,"src","fresh",0)); cold(&m);
    CHECK(same(&l,&io,0,"src",NULL) && same(&l,&io,0,"fresh",a) &&
          same(&l,&io,0,"dst",b) && same(&l,&io,20,"src",a));
    names=(struct names){{"alias","dst","fresh",NULL},0,{0},0};
    CHECK(!infs20_paged_volume_list(&l,&io,0,0,NULL,4,listed,&names,
        &listed_count,&more,&listed_sequence));
    CHECK(names.seen==3 && listed_count==3 && !more);
    names=(struct names){{"alias","dst","src",NULL},0,{0},0};
    CHECK(!infs20_paged_volume_list(&l,&io,20,0,NULL,4,listed,&names,
        &listed_count,&more,&listed_sequence));
    CHECK(names.seen==3 && listed_count==3 && !more);
    /* Many exclusive-scope edits must not consume a logical root per
     * operation or duplicate every unaffected namespace edge. Existing
     * bounded node capacity is only 32: forty aliases of one allocation
     * must fit without logical garbage collection. Metadata arena
     * collection is independent and can run between ordinary operations. */
    memset(m.disk,0,bytes); cold(&m);
    tx(id,3000); CHECK(!infs20_paged_volume_create(&l,&io,id)); cold(&m);
    tx(id,3001); CHECK(!infs20_paged_volume_write(&l,&io,id,"origin",a,LENGTH)); cold(&m);
    struct scope_check scope={0};
    CHECK(!infs20_paged_volume_recovery_visit(&l,&io,scope_mapping,scope_unit,&scope));
    CHECK(scope.initialised && scope.count==1);
    uint8_t original_scope[16]; memcpy(original_scope,scope.id,16);
    for(unsigned i=0;i<40;++i) {
        char name[16]; snprintf(name,sizeof(name),"alias-%02u",i);
        tx(id,3100+i); CHECK(!infs20_paged_volume_clone(&l,&io,id,"origin",name)); cold(&m);
        if(i%10==9) {
            struct infs20_paged_state gc;
            tx(id,3200+i); CHECK(!infs20_paged_collect(&l,&io,id,&gc)); cold(&m);
        }
    }
    scope.count=0;
    CHECK(!infs20_paged_volume_recovery_visit(&l,&io,scope_mapping,scope_unit,&scope));
    CHECK(scope.count==41 && !memcmp(scope.id,original_scope,16));
    CHECK(same(&l,&io,0,"alias-00",a) && same(&l,&io,0,"alias-39",a));
    /* Keep retained-scope fork bounded in this regression profile. */
    for(unsigned i=0;i<35;++i) {
        char name[16]; snprintf(name,sizeof(name),"alias-%02u",i);
        tx(id,3300+i); CHECK(!infs20_paged_volume_unlink(&l,&io,id,name)); cold(&m);
        if(i%10==9) {
            struct infs20_paged_state gc;
            tx(id,3400+i); CHECK(!infs20_paged_collect(&l,&io,id,&gc)); cold(&m);
        }
    }
    scope.count=0;
    CHECK(!infs20_paged_volume_recovery_visit(&l,&io,scope_mapping,scope_unit,&scope));
    CHECK(scope.count==6 && !memcmp(scope.id,original_scope,16));
    tx(id,3500); CHECK(!infs20_paged_volume_snapshot(&l,&io,id,31)); cold(&m);
    tx(id,3501); CHECK(!infs20_paged_volume_rename(&l,&io,id,"origin","renamed",0)); cold(&m);
    struct scope_check forked={0};
    CHECK(!infs20_paged_volume_recovery_visit(&l,&io,scope_mapping,scope_unit,&forked));
    CHECK(forked.count==6 && memcmp(forked.id,original_scope,16));
    CHECK(same(&l,&io,31,"origin",a) && same(&l,&io,31,"renamed",NULL));
    CHECK(same(&l,&io,0,"origin",NULL) && same(&l,&io,0,"renamed",a));
    tx(id,3502); CHECK(!infs20_paged_volume_write(&l,&io,id,"renamed",b,LENGTH)); cold(&m);
    forked.count=0;
    CHECK(!infs20_paged_volume_recovery_visit(&l,&io,scope_mapping,scope_unit,&forked));
    CHECK(forked.count==6 && same(&l,&io,0,"renamed",b) &&
          same(&l,&io,31,"origin",a) && same(&l,&io,0,"alias-39",a));
    free(raw.p); free(baseline); free(m.pending); free(m.disk);
    printf("format20-paged-volume: PASS (%u actual typed ownership/payload interruption cases)\n",cases);
    return 0;
}
