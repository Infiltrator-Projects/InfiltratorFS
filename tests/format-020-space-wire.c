// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/space_wire.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define N 160u
#define CHECK(x) do { if(!(x)){fprintf(stderr,"format20-space-wire:%d: %s\n",__LINE__,#x);return 1;} }while(0)
struct image { uint8_t block[N][INFS20_PAGE_BYTES]; };
static infs_status rd(void *ctx,uint64_t b,uint8_t *p)
{
    struct image *d=ctx;
    if(b>=N) return INFS_STATUS_IO_ERROR;
    memcpy(p,d->block[b],INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *ctx,uint64_t b,const uint8_t *p)
{
    struct image *d=ctx;
    if(b>=N) return INFS_STATUS_IO_ERROR;
    memcpy(d->block[b],p,INFS20_PAGE_BYTES); return 0;
}
static infs_status fl(void *ctx) { (void)ctx; return 0; }
static int stage(struct infs20_io *io,const struct infs20_space *s,
                 const struct infs20_authority *prior,struct infs20_commit *c)
{
    struct infs20_space_wire w;
    if(infs20_space_pack(s,&w)) return 1;
    memset(c,0,sizeof(*c)); c->blocks=N; c->previous_sequence=s->sequence-1;
    if(prior) memcpy(c->previous_digest,prior->digest,32);
    uint8_t empty[INFS20_PAYLOAD_BYTES]; size_t used;
    if(infs20_tree_encode(empty,sizeof(empty),0,NULL,0,&used)) return 1;
    /* Exactly the eight append reservations just recorded in the arena
     * directory. No metadata page address overlaps data or commit slots. */
    for(unsigned r=0;r<INFS20_ROOTS;++r) {
        struct infs20_ref *ref=&c->roots[r];
        ref->id[0]=(uint8_t)(r+1); ref->id[1]=(uint8_t)s->sequence;
        for(unsigned l=0;l<3;++l) {
            struct infs20_page_identity id={0};
            memcpy(id.set_id,io->set_id,16); memcpy(id.scope_id,io->set_id,16);
            memcpy(id.member_id,io->member_id,16); memcpy(id.page_id,ref->id,16);
            id.sequence=s->sequence; id.lane=(uint16_t)l;
            id.block=s->arena[l].physical.start+s->arena[l].frontier-INFS20_ROOTS+r;
            ref->blocks[l]=id.block;
            uint8_t page[INFS20_PAGE_BYTES];
            if(infs20_page_encode_kind(page,sizeof(page),&id,INFS20_TREE_KIND,
               r<2?empty:w.payload[r-2],r<2?used:w.size[r-2])) return 1;
            memcpy(ref->digest,page+INFS20_OFF_DIGEST,32);
            if(io->write(io->context,id.block,page)) return 1;
        }
    }
    return 0;
}
static int run(void)
{
    struct image *im=calloc(1,sizeof(*im)); CHECK(im);
    struct infs20_io io={0};
    io.context=im; io.read=rd; io.write=wr; io.flush=fl; io.blocks=N;
    io.set_id[0]=1; io.member_id[0]=2;
    for(unsigned b=0;b<2;++b) for(unsigned l=0;l<3;++l) io.slots[b][l]=b*3+l;
    const struct infs20_model_range arenas[3]={{8,24},{64,24},{120,24}};
    struct infs20_space a,b,reopened;
    struct infs20_commit c;
    struct infs20_authority first,second,selected;
    uint8_t id1[16]={90},id2[16]={91},data[16]={80},src[16]={81},clone[16]={82},digest[32]={83};
    uint64_t starts[3];
    CHECK(!infs20_space_init(&a,N,6,arenas));
    CHECK(!infs20_space_add(&a,data,digest,8192,(struct infs20_model_range){40,2},src,0));
    CHECK(!infs20_space_link(&a,data,clone,0));
    CHECK(!infs20_space_arena_reserve(&a,8,starts));
    CHECK(!stage(&io,&a,NULL,&c));
    CHECK(!infs20_publish(&io,NULL,&c,id1,100,&first));
    CHECK(!infs20_select(&io,100,&selected));
    CHECK(!infs20_space_read(&io,&selected,100,&reopened));
    CHECK(reopened.allocations==1 && reopened.alloc[0].references==2 &&
          reopened.alloc[0].physical.start==40);
    CHECK(!infs20_space_begin(&reopened,&b));
    CHECK(!infs20_space_relocate(&b,data,(struct infs20_model_range){50,2}));
    CHECK(!infs20_space_unlink(&b,src,0));
    CHECK(!infs20_space_arena_reserve(&b,8,starts));
    CHECK(!stage(&io,&b,&first,&c));
    CHECK(!infs20_publish(&io,&first,&c,id2,100,&second));
    CHECK(!infs20_select(&io,100,&selected) && selected.sequence==2);
    CHECK(!infs20_space_read(&io,&selected,100,&reopened));
    CHECK(reopened.alloc[0].physical.start==50 && reopened.alloc[0].references==1 &&
          reopened.retired==1 && reopened.old[0].physical.start==40);
    CHECK(!infs20_space_read(&io,&first,100,&reopened));
    CHECK(reopened.alloc[0].physical.start==40 && reopened.alloc[0].references==2);
    /* A corrupted derived owner index is refused even if framing is valid. */
    struct infs20_space_wire wire;
    CHECK(!infs20_space_pack(&b,&wire));
    wire.payload[2][8+18+1]^=1;
    CHECK(infs20_space_unpack(&wire,N,2,&reopened)==INFS_STATUS_CORRUPT);
    CHECK(!infs20_space_pack(&b,&wire));
    CHECK(infs20_space_unpack(&wire,N,1,&reopened)==INFS_STATUS_CORRUPT);
    CHECK(!infs20_space_unpack(&wire,N,2,&reopened));
    CHECK(infs20_space_advance_pin(&reopened,2)==INFS_STATUS_INVALID_ARGUMENT);
    /* Lose two entire lanes of generation 2. A mixed-copy read still
     * reconstructs exact references and retired physical ranges. */
    for(unsigned r=0;r<INFS20_ROOTS;++r)
        for(unsigned l=0;l<2;++l) memset(im->block[c.roots[r].blocks[l]],0,4096);
    memset(im->block[3],0,4096); memset(im->block[4],0,4096);
    CHECK(!infs20_select(&io,100,&selected) && selected.sequence==2);
    CHECK(!infs20_space_read(&io,&selected,100,&reopened));
    CHECK(reopened.alloc[0].physical.start==50 && reopened.retired==1);
    free(im);
    puts("format20-space-wire: PASS (persisted bounded ownership model)");
    return 0;
}
int main(void) { return run(); }
