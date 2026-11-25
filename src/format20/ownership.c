// SPDX-License-Identifier: GPL-3.0-or-later
#include "ownership.h"
#include "infilfs/endian.h"
#include <stdlib.h>
#include <string.h>

enum { NODE_KEY=17, NODE_VALUE=32, EDGE_KEY=25, EDGE_VALUE=24 };
static int zero(const uint8_t *p, size_t n)
{
    uint8_t v=0;
    while(n--) v|=*p++;
    return v==0;
}
static int find_node(const struct infs20_model_graph *g,const uint8_t id[16])
{
    for(size_t i=0;i<g->node_count;++i)
        if(!memcmp(g->nodes[i].id,id,16)) return (int)i;
    return -1;
}
static int find_pin(const struct infs20_model_graph *g,uint64_t ordinal)
{
    for(size_t i=0;i<g->edge_count;++i)
        if(zero(g->edges[i].source,16) && g->edges[i].ordinal==ordinal)
            return (int)i;
    return -1;
}
static int active(const struct infs20_model_graph *g)
{
    for(size_t i=0;i<g->edge_count;++i)
        if(g->edges[i].role==INFS20_MODEL_ACTIVE) return (int)i;
    return -1;
}
static int overlap(uint64_t a,uint64_t an,uint64_t b,uint64_t bn)
{
    /* Inputs have already been bounded by member capacity. */
    return an && bn && a<b+bn && b<a+an;
}
infs_status infs20_model_validate(const struct infs20_store_layout *l,
                                  const struct infs20_model_graph *g)
{
    if(!l || !g || g->node_count>INFS20_MODEL_NODES ||
       g->edge_count>INFS20_MODEL_EDGES || !l->member_blocks ||
       l->member_blocks>INFS20_MAX_MEMBER_BLOCKS ||
       l->member_blocks<INFS20_STORE_LANE_BLOCKS)
        return INFS_STATUS_INVALID_ARGUMENT;
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i) {
        if(l->lane_start[i]>l->member_blocks-INFS20_STORE_LANE_BLOCKS)
            return INFS_STATUS_INVALID_ARGUMENT;
        for(unsigned j=0;j<i;++j)
            if(overlap(l->lane_start[i],INFS20_STORE_LANE_BLOCKS,
                       l->lane_start[j],INFS20_STORE_LANE_BLOCKS))
                return INFS_STATUS_INVALID_ARGUMENT;
    }
    uint64_t incoming[INFS20_MODEL_NODES]={0};
    unsigned outgoing[INFS20_MODEL_NODES]={0};
    unsigned actives=0;
    for(size_t i=0;i<g->node_count;++i) {
        const struct infs20_model_node *n=&g->nodes[i];
        if(zero(n->id,16) || n->kind<INFS20_MODEL_ROOT ||
           n->kind>INFS20_MODEL_DATA || n->state<INFS20_MODEL_LIVE ||
           n->state>INFS20_MODEL_RELEASABLE ||
           (n->kind==INFS20_MODEL_DATA && (n->rank || !n->blocks)) ||
           (n->kind!=INFS20_MODEL_DATA && !n->rank) ||
           (!n->blocks && n->start) ||
           (n->blocks && (n->start>=l->member_blocks ||
                          n->blocks>l->member_blocks-n->start)))
            return INFS_STATUS_CORRUPT;
        for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
            if(overlap(n->start,n->blocks,l->lane_start[lane],
                       INFS20_STORE_LANE_BLOCKS))
                return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<i;++j)
            if(!memcmp(n->id,g->nodes[j].id,16) ||
               overlap(n->start,n->blocks,g->nodes[j].start,g->nodes[j].blocks))
                return INFS_STATUS_CORRUPT;
    }
    for(size_t i=0;i<g->edge_count;++i) {
        const struct infs20_model_edge *e=&g->edges[i];
        int dst=find_node(g,e->target);
        if(dst<0 || !e->ordinal || e->role>INFS20_MODEL_OPERATION)
            return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<i;++j)
            if(e->ordinal==g->edges[j].ordinal &&
               !memcmp(e->source,g->edges[j].source,16))
                return INFS_STATUS_CORRUPT;
        if(zero(e->source,16)) {
            if(e->role==INFS20_MODEL_STRONG ||
               g->nodes[dst].kind!=INFS20_MODEL_ROOT)
                return INFS_STATUS_CORRUPT;
            if(e->role==INFS20_MODEL_ACTIVE) ++actives;
        } else {
            int src=find_node(g,e->source);
            /* A strictly descending rank is a locally checkable DAG proof.
             * Root/namespace links are not unrestricted ownership edges. */
            if(src<0 || e->role!=INFS20_MODEL_STRONG ||
               g->nodes[src].kind==INFS20_MODEL_DATA ||
               g->nodes[src].rank<=g->nodes[dst].rank)
                return INFS_STATUS_CORRUPT;
            ++outgoing[src];
        }
        ++incoming[dst];
    }
    if(actives!=1) return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<g->node_count;++i) {
        const struct infs20_model_node *n=&g->nodes[i];
        if(n->refs!=incoming[i] ||
           ((n->state==INFS20_MODEL_LIVE)!=(incoming[i]!=0)) ||
           (n->state==INFS20_MODEL_RELEASABLE && outgoing[i]))
            return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}
static void remove_edge(struct infs20_model_graph *g,size_t i)
{
    /* Validated input proves that the edge exists and its target has a
     * positive direct-reference count. Drop edge and count together in the
     * private graph, then publish them in one committed metadata tree. */
    int dst=find_node(g,g->edges[i].target);
    struct infs20_model_node *n=&g->nodes[dst];
    if(--n->refs==0) n->state=INFS20_MODEL_PENDING;
    memmove(g->edges+i,g->edges+i+1,(g->edge_count-i-1)*sizeof(g->edges[0]));
    --g->edge_count;
}
static infs_status finish(const struct infs20_store_layout *l,
    struct infs20_model_graph *g,const struct infs20_model_graph *next)
{
    infs_status st=infs20_model_validate(l,next);
    if(!st) *g=*next;
    return st;
}
infs_status infs20_model_retire_activate(const struct infs20_store_layout *l,
    struct infs20_model_graph *g,const uint8_t new_root[16],uint64_t ordinal)
{
    infs_status st=infs20_model_validate(l,g);
    if(st) return st;
    if(!new_root || zero(new_root,16) || !ordinal ||
       find_pin(g,ordinal)>=0 || find_node(g,new_root)>=0)
        return INFS_STATUS_INVALID_ARGUMENT;
    if(g->node_count==INFS20_MODEL_NODES || g->edge_count==INFS20_MODEL_EDGES)
        return INFS_STATUS_NO_SPACE;
    struct infs20_model_graph next=*g;
    int a=active(&next);
    struct infs20_model_edge *r=&next.edges[next.edge_count++];
    memset(r,0,sizeof(*r));
    memcpy(r->target,next.edges[a].target,16);
    r->ordinal=ordinal; r->role=INFS20_MODEL_RETIRED;
    /* The former active pin becomes the retained pin: net old refs unchanged.
     * New empty root has one incoming active pin and no child ownership. */
    struct infs20_model_node *n=&next.nodes[next.node_count++];
    memset(n,0,sizeof(*n)); memcpy(n->id,new_root,16);
    n->rank=2; n->kind=INFS20_MODEL_ROOT; n->state=INFS20_MODEL_LIVE; n->refs=1;
    memcpy(next.edges[a].target,new_root,16);
    return finish(l,g,&next);
}
infs_status infs20_model_reactivate(const struct infs20_store_layout *l,
    struct infs20_model_graph *g,uint64_t ordinal)
{
    infs_status st=infs20_model_validate(l,g);
    if(st) return st;
    int r=find_pin(g,ordinal);
    if(r<0 || g->edges[r].role!=INFS20_MODEL_RETIRED)
        return INFS_STATUS_NOT_FOUND;
    struct infs20_model_graph next=*g;
    int a=active(&next);
    uint8_t former[16];
    memcpy(former,next.edges[a].target,16);
    memcpy(next.edges[a].target,next.edges[r].target,16);
    memcpy(next.edges[r].target,former,16);
    /* Both roots remain strongly pinned; no extent or checksum is copied. */
    return finish(l,g,&next);
}
infs_status infs20_model_drop_pin(const struct infs20_store_layout *l,
    struct infs20_model_graph *g,uint64_t ordinal)
{
    infs_status st=infs20_model_validate(l,g);
    if(st) return st;
    int i=find_pin(g,ordinal);
    if(i<0) return INFS_STATUS_NOT_FOUND;
    if(g->edges[i].role==INFS20_MODEL_ACTIVE) return INFS_STATUS_BUSY;
    struct infs20_model_graph next=*g;
    remove_edge(&next,(size_t)i);
    return finish(l,g,&next);
}
infs_status infs20_model_reclaim_step(const struct infs20_store_layout *l,
    struct infs20_model_graph *g,const uint8_t id[16])
{
    infs_status st=infs20_model_validate(l,g);
    if(st) return st;
    if(!id) return INFS_STATUS_INVALID_ARGUMENT;
    int n=find_node(g,id);
    if(n<0) return INFS_STATUS_NOT_FOUND;
    if(g->nodes[n].state==INFS20_MODEL_LIVE) return INFS_STATUS_BUSY;
    if(g->nodes[n].state==INFS20_MODEL_RELEASABLE) return INFS_STATUS_OK;
    struct infs20_model_graph next=*g;
    /* Remove exactly one outgoing edge. The durable edge set is the cursor:
     * replaying from a committed graph cannot remove that edge twice. */
    size_t chosen=next.edge_count;
    for(size_t i=0;i<next.edge_count;++i)
        if(!memcmp(next.edges[i].source,id,16) &&
           (chosen==next.edge_count ||
            next.edges[i].ordinal<next.edges[chosen].ordinal)) chosen=i;
    if(chosen<next.edge_count) {
        remove_edge(&next,chosen);
        next.nodes[n].state=INFS20_MODEL_DRAINING;
    } else next.nodes[n].state=INFS20_MODEL_RELEASABLE;
    return finish(l,g,&next);
}
infs_status infs20_model_release(const struct infs20_store_layout *l,
    struct infs20_model_graph *g,const uint8_t id[16],int retirement_proven)
{
    infs_status st=infs20_model_validate(l,g);
    if(st) return st;
    if(!id) return INFS_STATUS_INVALID_ARGUMENT;
    int i=find_node(g,id);
    if(i<0) return INFS_STATUS_NOT_FOUND;
    if(!retirement_proven || g->nodes[i].state!=INFS20_MODEL_RELEASABLE)
        return INFS_STATUS_BUSY;
    struct infs20_model_graph next=*g;
    memmove(next.nodes+i,next.nodes+i+1,
            (next.node_count-(size_t)i-1)*sizeof(next.nodes[0]));
    --next.node_count;
    return finish(l,g,&next);
}

/* Canonical prototype records use a family byte and opaque identity key.
 * Numeric fields are explicit little-endian. Keys are sorted by the bounded
 * store's byte comparator; ordinal bytes are opaque at this layer, while
 * reclaim uses decoded numeric order. No host struct is persisted. */
struct encoded {
    uint8_t key[EDGE_KEY], value[NODE_VALUE];
    uint16_t key_size,value_size;
};
static int compare_encoded(const void *a,const void *b)
{
    const struct encoded *x=a,*y=b;
    size_t n=x->key_size<y->key_size?x->key_size:y->key_size;
    int c=memcmp(x->key,y->key,n);
    return c?c:(x->key_size>y->key_size)-(x->key_size<y->key_size);
}
static int compare_records(const void *a,const void *b)
{
    const struct infs20_store_record *x=a,*y=b;
    size_t n=x->key_size<y->key_size?x->key_size:y->key_size;
    int c=memcmp(x->key,y->key,n);
    return c?c:(x->key_size>y->key_size)-(x->key_size<y->key_size);
}
infs_status infs20_model_with_records(const struct infs20_store_layout *l,
    const struct infs20_model_graph *g,const struct infs20_store_record *extra,
    size_t extra_count,infs20_model_records_fn visit,void *context)
{
    if(!visit) return INFS_STATUS_INVALID_ARGUMENT;
    infs_status st=infs20_model_validate(l,g);
    if(st) return st;
    struct encoded enc[INFS20_MODEL_NODES+INFS20_MODEL_EDGES]={0};
    if(extra_count>128 || (extra_count && !extra)) return INFS_STATUS_INVALID_ARGUMENT;
    for(size_t i=0;i<extra_count;++i)
        if(!extra[i].key || !extra[i].key_size ||
           extra[i].key_size>INFS20_STORE_KEY_MAX ||
           (extra[i].value_size && !extra[i].value) ||
           extra[i].key[0]=='N' || extra[i].key[0]=='E')
            return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_store_record r[INFS20_MODEL_NODES+INFS20_MODEL_EDGES+128];
    size_t count=0;
    for(size_t i=0;i<g->node_count;++i) {
        const struct infs20_model_node *n=&g->nodes[i];
        struct encoded *e=&enc[count++];
        e->key[0]='N'; memcpy(e->key+1,n->id,16); e->key_size=NODE_KEY;
        e->value[0]=1; e->value[1]=n->kind; e->value[2]=n->state;
        infs_store_le32(e->value+4,n->rank);
        infs_store_le64(e->value+8,n->refs);
        infs_store_le64(e->value+16,n->start);
        infs_store_le64(e->value+24,n->blocks);
        e->value_size=NODE_VALUE;
    }
    for(size_t i=0;i<g->edge_count;++i) {
        const struct infs20_model_edge *edge=&g->edges[i];
        struct encoded *e=&enc[count++];
        e->key[0]='E'; memcpy(e->key+1,edge->source,16);
        infs_store_le64(e->key+17,edge->ordinal); e->key_size=EDGE_KEY;
        e->value[0]=1; e->value[1]=edge->role;
        memcpy(e->value+8,edge->target,16); e->value_size=EDGE_VALUE;
    }
    qsort(enc,count,sizeof(enc[0]),compare_encoded);
    for(size_t i=0;i<count;++i)
        r[i]=(struct infs20_store_record){enc[i].key,enc[i].value,
                                         enc[i].key_size,enc[i].value_size};
    for(size_t i=0;i<extra_count;++i) r[count++]=extra[i];
    qsort(r,count,sizeof(r[0]),compare_records);
    for(size_t i=1;i<count;++i)
        if(!compare_records(r+i-1,r+i)) return INFS_STATUS_INVALID_ARGUMENT;
    return visit(context,r,count);
}
struct bounded_publish {
    const struct infs20_store_layout *layout;
    const struct infs20_store_io *io;
    const uint8_t *tx;
    infs20_store_prepare prepare;
    void *context;
    struct infs20_store_state *out;
};
static infs_status publish_bounded(void *opaque,
    const struct infs20_store_record *r,size_t count)
{
    struct bounded_publish *p=opaque;
    return infs20_store_publish_prepared(p->layout,p->io,p->tx,r,count,
                                        p->prepare,p->context,p->out);
}
infs_status infs20_model_commit_extended(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    const struct infs20_model_graph *g,const struct infs20_store_record *extra,
    size_t extra_count,infs20_store_prepare prepare,void *context,
    struct infs20_store_state *out)
{
    struct bounded_publish p={l,io,tx,prepare,context,out};
    return infs20_model_with_records(l,g,extra,extra_count,publish_bounded,&p);
}
infs_status infs20_model_commit(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    const struct infs20_model_graph *g,struct infs20_store_state *out)
{
    return infs20_model_commit_extended(l,io,tx,g,NULL,0,NULL,NULL,out);
}
infs_status infs20_model_decode_record(void *opaque,const struct infs20_record_view *r)
{
    struct infs20_model_graph *g=opaque;
    if(r->key[0]=='N') {
        if(r->key_size!=NODE_KEY || r->value_size!=NODE_VALUE ||
           r->value[0]!=1 || r->value[3] ||
           g->node_count==INFS20_MODEL_NODES) return INFS_STATUS_CORRUPT;
        struct infs20_model_node *n=&g->nodes[g->node_count++];
        memcpy(n->id,r->key+1,16); n->kind=r->value[1]; n->state=r->value[2];
        n->rank=infs_load_le32(r->value+4);
        n->refs=infs_load_le64(r->value+8);
        n->start=infs_load_le64(r->value+16);
        n->blocks=infs_load_le64(r->value+24);
    } else if(r->key[0]=='E') {
        if(r->key_size!=EDGE_KEY || r->value_size!=EDGE_VALUE ||
           r->value[0]!=1 || !zero(r->value+2,6) ||
           g->edge_count==INFS20_MODEL_EDGES) return INFS_STATUS_CORRUPT;
        struct infs20_model_edge *e=&g->edges[g->edge_count++];
        memcpy(e->source,r->key+1,16); e->ordinal=infs_load_le64(r->key+17);
        e->role=r->value[1]; memcpy(e->target,r->value+8,16);
    } else return INFS_STATUS_NOT_SUPPORTED;
    return INFS_STATUS_OK;
}
infs_status infs20_model_load(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,struct infs20_model_graph *out)
{
    if(!out) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_model_graph g={0};
    infs_status st=infs20_store_visit(l,io,infs20_model_decode_record,&g);
    if(!st) st=infs20_model_validate(l,&g);
    if(!st) *out=g;
    return st;
}
