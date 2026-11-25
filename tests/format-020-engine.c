// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/engine.h"
#include "infilfs/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N 128u
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"format20-engine:%d: %s\n",__LINE__,#x); return 1; } } while (0)

/* Volatile cache and durable media are separate. A flush failure may occur
 * before or after the write reaches media; a write may tear. No real device. */
struct disk {
    uint8_t stable[N][INFS20_PAGE_BYTES], cache[N][INFS20_PAGE_BYTES];
    unsigned step, fail_at, mode;
};
static infs_status rd(void *ctx,uint64_t b,uint8_t *p)
{
    struct disk *d=ctx;
    if (b>=N) return INFS_STATUS_IO_ERROR;
    memcpy(p,d->cache[b],INFS20_PAGE_BYTES);
    return 0;
}
static infs_status wr(void *ctx,uint64_t b,const uint8_t *p)
{
    struct disk *d=ctx;
    if (b>=N) return INFS_STATUS_IO_ERROR;
    if (++d->step==d->fail_at) {
        if (d->mode==2) memcpy(d->stable[b],p,256);
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(d->cache[b],p,INFS20_PAGE_BYTES);
    return 0;
}
static infs_status fl(void *ctx)
{
    struct disk *d=ctx;
    if (++d->step==d->fail_at) {
        if (d->mode==1) memcpy(d->stable,d->cache,sizeof(d->stable));
        return INFS_STATUS_IO_ERROR;
    }
    memcpy(d->stable,d->cache,sizeof(d->stable)); return 0;
}
static void cold(struct disk *d)
{
    memcpy(d->cache,d->stable,sizeof(d->cache)); d->step=0; d->fail_at=0;
}
static void init_io(struct infs20_io *io,struct disk *d)
{
    memset(io,0,sizeof(*io)); io->context=d; io->read=rd; io->write=wr;
    io->flush=fl; io->blocks=N; io->set_id[0]=1; io->member_id[0]=2;
    for(unsigned b=0;b<2;++b) for(unsigned l=0;l<3;++l)
        io->slots[b][l]=b*3+l;
}
static int make_tree(struct infs20_io *io,struct infs20_ref *r,
                     uint64_t seq,unsigned start,uint8_t idbyte,
                     uint16_t level,const struct infs20_tree_entry *e,size_t count)
{
    struct disk *d=io->context;
    uint8_t payload[INFS20_PAYLOAD_BYTES]; size_t used=0;
    if (infs20_tree_encode(payload,sizeof(payload),level,e,count,&used)) return 1;
    memset(r,0,sizeof(*r)); r->id[0]=idbyte;
    for(unsigned l=0;l<3;++l) {
        struct infs20_page_identity id={0};
        memcpy(id.set_id,io->set_id,16); memcpy(id.scope_id,io->set_id,16);
        memcpy(id.member_id,io->member_id,16); memcpy(id.page_id,r->id,16);
        id.sequence=seq; id.block=start+l; id.lane=(uint16_t)l;
        r->blocks[l]=id.block;
        uint8_t *page=d->cache[id.block];
        if (infs20_page_encode_kind(page,INFS20_PAGE_BYTES,&id,INFS20_TREE_KIND,
                                    payload,used)) return 1;
        memcpy(r->digest,page+INFS20_OFF_DIGEST,32);
    }
    return 0;
}
static int build_roots(struct infs20_io *io,struct infs20_commit *c,
                       uint64_t seq,unsigned start)
{
    memset(c,0,sizeof(*c)); c->blocks=N; c->previous_sequence=seq-1;
    for(unsigned i=0;i<INFS20_ROOTS;++i)
        if (make_tree(io,&c->roots[i],seq,start+3*i,(uint8_t)(10+start+i),0,NULL,0))
            return 1;
    return 0;
}
static int run(void)
{
    struct disk *d=calloc(1,sizeof(*d)), *baseline=malloc(sizeof(*d));
    CHECK(d && baseline);
    struct infs20_io io; init_io(&io,d);
    struct infs20_commit first,next;
    struct infs20_authority a,b;
    uint8_t id1[16]={90}, id2[16]={91};
    CHECK(infs20_select(&io,100,&a)==INFS_STATUS_NOT_FOUND);
    CHECK(!build_roots(&io,&first,1,6));
    CHECK(!infs20_publish(&io,NULL,&first,id1,100,&a));
    CHECK(a.sequence==1 && a.lane_mask==7 && a.bank==0);
    cold(d);
    CHECK(!infs20_select(&io,100,&b) && b.sequence==1);
    CHECK(!memcmp(a.digest,b.digest,32));
    CHECK(infs20_select(&io,0,&b)==INFS_STATUS_BUSY);
    *baseline=*d;
    /* Every interruption of dependency flush, commit write or commit flush,
     * with distinct before/after durability and torn-write cases. */
    for(unsigned mode=0;mode<3;++mode) for(unsigned cut=1;cut<=7;++cut) {
        *d=*baseline;
        CHECK(!build_roots(&io,&next,2,30));
        memcpy(next.previous_digest,a.digest,32);
        d->fail_at=cut; d->mode=mode;
        memset(&b,0xa5,sizeof(b));
        CHECK(infs20_publish(&io,&a,&next,id2,100,&b)==INFS_STATUS_IO_ERROR);
        cold(d);
        CHECK(!infs20_select(&io,100,&b));
        CHECK(b.sequence==1 || b.sequence==2);
        if (cut<=2) CHECK(b.sequence==1);
        if (cut>=4) CHECK(b.sequence==2);
        if (b.sequence==2) CHECK(b.bank==1 && b.lane_mask!=0);
    }
    *d=*baseline;
    CHECK(!build_roots(&io,&next,2,30));
    memcpy(next.previous_digest,a.digest,32);
    CHECK(!infs20_publish(&io,&a,&next,id2,100,&b));
    CHECK(b.sequence==2 && b.lane_mask==7 && b.bank==1);
    cold(d);
    CHECK(infs20_publish(&io,&a,&next,id2,100,&b)==INFS_STATUS_BUSY);
    /* Lose two copies of every new root and two commit copies: remaining
     * graph still proves generation 2. Destroy final proof: never roll back. */
    for(unsigned i=0;i<INFS20_ROOTS;++i)
        for(unsigned l=0;l<2;++l) memset(d->cache[next.roots[i].blocks[l]],0,4096);
    memset(d->cache[3],0,4096); memset(d->cache[4],0,4096);
    CHECK(!infs20_select(&io,100,&b) && b.sequence==2 && b.lane_mask==4);
    memset(d->cache[next.roots[0].blocks[2]],0,4096);
    CHECK(infs20_select(&io,100,&b)==INFS_STATUS_CORRUPT);

    /* Actual two-child tree: prove minima, ordering, level and digest.
     * Reuse a leaf for the wrong key range and reject it. */
    *d=*baseline;
    struct infs20_ref left,right,root;
    const uint8_t val[]={7};
    struct infs20_tree_entry leaves[2]={{{1},val,1},{{2},val,1}};
    CHECK(!make_tree(&io,&left,2,60,101,0,&leaves[0],1));
    CHECK(!make_tree(&io,&right,2,63,102,0,&leaves[1],1));
    uint8_t refs[2][INFS20_REF_BYTES];
    CHECK(!infs20_ref_encode(refs[0],&left,N));
    CHECK(!infs20_ref_encode(refs[1],&right,N));
    struct infs20_tree_entry branch[2]={{{1},refs[0],INFS20_REF_BYTES},
                                        {{2},refs[1],INFS20_REF_BYTES}};
    CHECK(!make_tree(&io,&root,2,66,103,1,branch,2));
    size_t budget=100;
    CHECK(!infs20_verify_tree(&io,&root,2,&budget));
    CHECK(!infs20_ref_encode(refs[1],&left,N));
    CHECK(!make_tree(&io,&root,2,69,104,1,branch,2));
    budget=100;
    CHECK(infs20_verify_tree(&io,&root,2,&budget)==INFS_STATUS_CORRUPT);
    free(baseline); free(d);
    puts("format20-engine: PASS (experimental tree/commit interruption model)");
    return 0;
}
int main(void) { return run(); }
