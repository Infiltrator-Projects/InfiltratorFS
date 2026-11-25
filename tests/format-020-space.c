// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/space.h"
#include <stdio.h>
#include <string.h>
#define CHECK(x) do { if(!(x)){fprintf(stderr,"format20-space:%d: %s\n",__LINE__,#x);return 1;} }while(0)
int main(void)
{
    struct infs20_space current,next,saved,third;
    const struct infs20_model_range arenas[3]={{8,8},{64,8},{120,8}};
    uint8_t a[16]={1},b[16]={2},c[16]={3},owner1[16]={9},owner2[16]={10},
            owner3[16]={11},digest[32]={42};
    uint64_t starts[3];
    size_t released;
    CHECK(!infs20_space_init(&current,160,6,arenas));
    CHECK(!infs20_space_arena_reserve(&current,3,starts));
    CHECK(starts[0]==8 && starts[1]==64 && starts[2]==120);
    saved=current;
    CHECK(infs20_space_arena_reserve(&current,6,starts)==INFS_STATUS_NO_SPACE);
    CHECK(!memcmp(&saved,&current,sizeof(current)));
    CHECK(!infs20_space_add(&current,a,digest,8192,(struct infs20_model_range){20,2},owner1,0));
    CHECK(!infs20_space_link(&current,a,owner2,0));
    CHECK(current.alloc[0].references==2 && current.edges==2);
    CHECK(infs20_space_link(&current,a,owner2,0)==INFS_STATUS_ALREADY_EXISTS);
    CHECK(!infs20_space_validate(&current));
    saved=current;
    CHECK(!infs20_space_begin(&current,&next));
    CHECK(!infs20_space_relocate(&next,a,(struct infs20_model_range){30,2}));
    CHECK(next.alloc[0].physical.start==30 && next.alloc[0].references==2 &&
          !memcmp(next.alloc[0].digest,digest,32));
    CHECK(!memcmp(&saved,&current,sizeof(current))); /* private transaction */
    CHECK(infs20_space_add(&next,b,digest,8192,(struct infs20_model_range){20,2},owner3,0)==INFS_STATUS_NO_SPACE);
    CHECK(!infs20_space_unlink(&next,owner1,0));
    CHECK(next.alloc[0].references==1 && next.alloc[0].state==INFS20_ALLOC_LIVE);
    CHECK(!infs20_space_unlink(&next,owner2,0));
    CHECK(next.alloc[0].state==INFS20_ALLOC_PENDING && next.alloc[0].references==0);
    CHECK(infs20_space_link(&next,a,owner1,1)==INFS_STATUS_NOT_FOUND);
    CHECK(!infs20_space_collect(&next,99,&released) && released==0);
    CHECK(next.allocations==1 && next.retired==1);
    CHECK(!infs20_space_begin(&next,&third));
    CHECK(!infs20_space_advance_pin(&third,2));
    CHECK(!infs20_space_collect(&third,1,&released) && released==1);
    CHECK(third.allocations==0 && third.retired==1);
    CHECK(!infs20_space_collect(&third,1,&released) && released==1);
    CHECK(!infs20_space_collect(&third,1,&released) && released==0);
    CHECK(!infs20_space_add(&third,b,digest,8192,(struct infs20_model_range){20,2},owner3,0));
    CHECK(!infs20_space_add(&third,c,digest,8192,(struct infs20_model_range){30,2},owner3,1));
    CHECK(!infs20_space_validate(&third));
    CHECK(infs20_space_add(&third,a,digest,4096,(struct infs20_model_range){64,1},owner1,0)==INFS_STATUS_NO_SPACE);
    CHECK(infs20_space_add(&third,a,digest,4096,(struct infs20_model_range){31,2},owner1,0)==INFS_STATUS_NO_SPACE);
    CHECK(infs20_space_add(&third,a,digest,4096,(struct infs20_model_range){159,2},owner1,0)==INFS_STATUS_NO_SPACE);
    saved=third;
    third.alloc[0].references++;
    CHECK(infs20_space_validate(&third)==INFS_STATUS_CORRUPT);
    third=saved;
    third.alloc[1].physical=third.alloc[0].physical;
    CHECK(infs20_space_validate(&third)==INFS_STATUS_CORRUPT);
    third=saved;
    third.edge[1]=third.edge[0];
    CHECK(infs20_space_validate(&third)==INFS_STATUS_CORRUPT);
    CHECK(infs20_space_advance_pin(&saved,1)==INFS_STATUS_INVALID_ARGUMENT);
    CHECK(!infs20_space_validate(&saved));
    puts("format20-space: PASS (bounded reference/relocation/arena model)");
    return 0;
}
