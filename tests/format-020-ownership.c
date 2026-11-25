// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/ownership.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-ownership:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { BLOCKS=96 };
struct medium { uint8_t disk[BLOCKS][INFS20_PAGE_BYTES]; unsigned writes; };
static infs_status rd(void *ctx,uint64_t b,uint8_t *p)
{
    struct medium *m=ctx;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    memcpy(p,m->disk[b],INFS20_PAGE_BYTES); return 0;
}
static infs_status wr(void *ctx,uint64_t b,const uint8_t *p)
{
    struct medium *m=ctx;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    ++m->writes; memcpy(m->disk[b],p,INFS20_PAGE_BYTES); return 0;
}
static infs_status fl(void *ctx) { (void)ctx; return 0; }
static void node(struct infs20_model_graph *g,unsigned id,unsigned rank,
                 unsigned kind,unsigned refs,uint64_t start,uint64_t blocks)
{
    struct infs20_model_node *n=&g->nodes[g->node_count++];
    memset(n,0,sizeof(*n)); n->id[0]=(uint8_t)id; n->rank=rank;
    n->kind=(uint8_t)kind; n->state=INFS20_MODEL_LIVE; n->refs=refs;
    n->start=start; n->blocks=blocks;
}
static void edge(struct infs20_model_graph *g,unsigned src,unsigned dst,
                 unsigned ordinal,unsigned role)
{
    struct infs20_model_edge *e=&g->edges[g->edge_count++];
    memset(e,0,sizeof(*e)); e->source[0]=(uint8_t)src; e->target[0]=(uint8_t)dst;
    e->ordinal=ordinal; e->role=(uint8_t)role;
}
static const struct infs20_model_node *get(const struct infs20_model_graph *g,unsigned id)
{
    for(size_t i=0;i<g->node_count;++i)
        if(g->nodes[i].id[0]==id) return &g->nodes[i];
    return NULL;
}
static unsigned current(const struct infs20_model_graph *g)
{
    for(size_t i=0;i<g->edge_count;++i)
        if(g->edges[i].role==INFS20_MODEL_ACTIVE) return g->edges[i].target[0];
    return 0;
}
static int persist(const struct infs20_store_layout *l,const struct infs20_store_io *io,
                   struct infs20_model_graph *g,unsigned seq)
{
    uint8_t tx[16]={0}; tx[0]=(uint8_t)seq; tx[1]=0x42;
    struct infs20_store_state s;
    CHECK(infs20_model_commit(l,io,tx,g,&s)==0 && s.fully_protected);
    memset(g,0xa5,sizeof(*g));
    CHECK(infs20_model_load(l,io,g)==0);
    return 0;
}
int main(void)
{
    struct medium *m=calloc(1,sizeof(*m)); CHECK(m);
    struct infs20_store_layout l={{1},{2},{3},BLOCKS,{0,32,64}};
    struct infs20_store_io io={m,rd,wr,fl};
    struct infs20_model_graph g={0};
    uint8_t first[16]={1}, second[16]={5}, page[16]={2}, data[16]={4};
    /* Two mappings share one data allocation. A snapshot and a fallback pin
     * retain the first root independently of its active/retired role. */
    node(&g,1,3,INFS20_MODEL_ROOT,3,0,0);
    node(&g,2,1,INFS20_MODEL_PAGE,1,22,1);
    node(&g,3,1,INFS20_MODEL_PAGE,1,23,1);
    node(&g,4,0,INFS20_MODEL_DATA,2,24,4);
    edge(&g,0,1,1,INFS20_MODEL_ACTIVE);
    edge(&g,0,1,2,INFS20_MODEL_SNAPSHOT);
    edge(&g,0,1,3,INFS20_MODEL_FALLBACK);
    edge(&g,1,2,1,INFS20_MODEL_STRONG);
    edge(&g,1,3,2,INFS20_MODEL_STRONG);
    edge(&g,2,4,1,INFS20_MODEL_STRONG);
    edge(&g,3,4,1,INFS20_MODEL_STRONG);
    CHECK(infs20_model_validate(&l,&g)==0);
    CHECK(persist(&l,&io,&g,1)==0);
    CHECK(infs20_model_retire_activate(&l,&g,second,4)==0);
    CHECK(current(&g)==5 && get(&g,1)->refs==3 && get(&g,4)->refs==2);
    CHECK(persist(&l,&io,&g,2)==0);
    CHECK(infs20_model_reactivate(&l,&g,4)==0);
    CHECK(current(&g)==1 && get(&g,5)->refs==1);
    CHECK(persist(&l,&io,&g,3)==0);
    CHECK(infs20_model_reactivate(&l,&g,4)==0 && current(&g)==5);
    CHECK(infs20_model_drop_pin(&l,&g,4)==0 && get(&g,1)->refs==2);
    CHECK(infs20_model_reclaim_step(&l,&g,first)==INFS_STATUS_BUSY);
    CHECK(infs20_model_drop_pin(&l,&g,2)==0 && get(&g,1)->refs==1);
    CHECK(infs20_model_reclaim_step(&l,&g,first)==INFS_STATUS_BUSY);
    CHECK(infs20_model_drop_pin(&l,&g,3)==0 && get(&g,1)->refs==0);
    CHECK(persist(&l,&io,&g,4)==0);
    CHECK(infs20_model_reclaim_step(&l,&g,first)==0);
    CHECK(get(&g,2)->refs==0 && get(&g,3)->refs==1);
    CHECK(persist(&l,&io,&g,5)==0);
    CHECK(infs20_model_reclaim_step(&l,&g,first)==0);
    CHECK(get(&g,2)->refs==0 && get(&g,3)->refs==0);
    CHECK(infs20_model_reclaim_step(&l,&g,page)==0 && get(&g,4)->refs==1);
    CHECK(persist(&l,&io,&g,6)==0);
    CHECK(infs20_model_reclaim_step(&l,&g,page)==0);
    CHECK(get(&g,4)->refs==1 && get(&g,2)->state==INFS20_MODEL_RELEASABLE);
    CHECK(infs20_model_release(&l,&g,page,0)==INFS_STATUS_BUSY);
    CHECK(infs20_model_release(&l,&g,page,1)==0 && !get(&g,2));
    uint8_t other[16]={3};
    CHECK(infs20_model_reclaim_step(&l,&g,other)==0 && get(&g,4)->refs==0);
    CHECK(get(&g,4)->state==INFS20_MODEL_PENDING);
    CHECK(infs20_model_reclaim_step(&l,&g,data)==0);
    CHECK(infs20_model_release(&l,&g,data,0)==INFS_STATUS_BUSY);
    CHECK(infs20_model_release(&l,&g,data,1)==0);
    CHECK(persist(&l,&io,&g,7)==0 && !get(&g,4) && current(&g)==5);
    /* Independently reject inconsistent ownership and forbid publication. */
    unsigned before=m->writes;
    struct infs20_model_graph bad=g;
    bad.nodes[0].refs++;
    struct infs20_store_state state;
    uint8_t tx[16]={8};
    CHECK(infs20_model_commit(&l,&io,tx,&bad,&state)==INFS_STATUS_CORRUPT);
    bad=g;
    node(&bad,6,0,INFS20_MODEL_DATA,1,32,1); /* overlaps metadata lane */
    edge(&bad,5,6,9,INFS20_MODEL_STRONG);
    CHECK(infs20_model_commit(&l,&io,tx,&bad,&state)==INFS_STATUS_CORRUPT);
    bad=g;
    edge(&bad,5,5,10,INFS20_MODEL_STRONG); /* cycle */
    CHECK(infs20_model_commit(&l,&io,tx,&bad,&state)==INFS_STATUS_CORRUPT);
    CHECK(m->writes==before);
    puts("format20-ownership: PASS (persistent root retention, shared edges, bounded reclaim)");
    free(m); return 0;
}
