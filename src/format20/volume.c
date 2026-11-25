// SPDX-License-Identifier: GPL-3.0-or-later
#include "volume.h"
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include <string.h>

/*
 * Every name mapping is an owning edge from an immutable namespace root to
 * a content allocation. Its descriptor owns the logical length and digest;
 * the graph's node range is the sole physical location authority. Reflinks
 * share both. This deliberately bounded model never reuses any committed
 * payload block, even after dropping a root: the previous commit bank and
 * interrupted readers cannot be invalidated by reclamation. General free
 * space reuse requires a separately qualified retirement/arena protocol.
 */
struct mapping {
    uint8_t root[16], data[16];
    char name[INFS20_VOLUME_NAME_MAX+1];
    uint64_t ordinal;
};
struct content {
    uint8_t id[16], digest[32];
    uint32_t size;
};
struct volume {
    struct infs20_model_graph graph;
    struct mapping files[INFS20_VOLUME_FILES];
    struct content contents[INFS20_MODEL_NODES];
    size_t files_count, contents_count;
    uint64_t frontier;
    unsigned header;
};
struct pending {
    const struct infs20_store_io *io;
    uint64_t block;
    uint8_t data[INFS20_PAGE_BYTES];
};
static int equal(const uint8_t *a,const uint8_t *b) { return !memcmp(a,b,16); }
static int zero(const uint8_t *a,size_t n)
{
    uint8_t v=0; while(n--) v|=*a++; return v==0;
}
static int name_valid(const char *name)
{
    if(!name || !*name) return 0;
    size_t n=0;
    for(;name[n];++n) {
        unsigned char c=(unsigned char)name[n];
        if(n==INFS20_VOLUME_NAME_MAX || c<0x20 || c>0x7e ||
           c=='/' || c=='\\') return 0;
    }
    return strcmp(name,".") && strcmp(name,"..");
}
static int node(const struct volume *v,const uint8_t id[16])
{
    for(size_t i=0;i<v->graph.node_count;++i)
        if(equal(v->graph.nodes[i].id,id)) return (int)i;
    return -1;
}
static int content(const struct volume *v,const uint8_t id[16])
{
    for(size_t i=0;i<v->contents_count;++i)
        if(equal(v->contents[i].id,id)) return (int)i;
    return -1;
}
static int pin(const struct volume *v,uint64_t ordinal)
{
    for(size_t i=0;i<v->graph.edge_count;++i) {
        const struct infs20_model_edge *e=&v->graph.edges[i];
        if(zero(e->source,16) &&
           (ordinal ? e->ordinal==ordinal : e->role==INFS20_MODEL_ACTIVE))
            return (int)i;
    }
    return -1;
}
static int file(const struct volume *v,const uint8_t root[16],const char *name)
{
    for(size_t i=0;i<v->files_count;++i)
        if(equal(v->files[i].root,root) && !strcmp(v->files[i].name,name))
            return (int)i;
    return -1;
}
static void id_make(const uint8_t tx[16],uint8_t tag,uint8_t out[16])
{
    uint8_t preimage[22]={'V','2','0','I','D',tag},digest[32];
    memcpy(preimage+6,tx,16); infs_sha256(preimage,sizeof(preimage),digest);
    memcpy(out,digest,16);
}
static int tx_valid(const uint8_t tx[16]) { return tx && !zero(tx,16); }
static infs_status validate(const struct infs20_store_layout *l,const struct volume *v)
{
    infs_status st=infs20_model_validate(l,&v->graph);
    if(st) return st;
    if(v->header!=1 || v->frontier>l->member_blocks ||
       v->files_count>INFS20_VOLUME_FILES || v->contents_count>INFS20_MODEL_NODES)
        return INFS_STATUS_CORRUPT;
    size_t data_count=0,strong_count=0;
    for(size_t i=0;i<v->graph.node_count;++i) {
        const struct infs20_model_node *n=&v->graph.nodes[i];
        if(n->kind==INFS20_MODEL_DATA) {
            ++data_count;
            int c=content(v,n->id);
            if(c<0 || n->blocks!=1 || n->start>=v->frontier ||
               v->contents[c].size>INFS20_PAGE_BYTES) return INFS_STATUS_CORRUPT;
        } else if(n->kind!=INFS20_MODEL_ROOT || n->blocks)
            return INFS_STATUS_CORRUPT;
    }
    if(data_count!=v->contents_count) return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<v->contents_count;++i)
        for(size_t j=0;j<i;++j)
            if(equal(v->contents[i].id,v->contents[j].id)) return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<v->graph.edge_count;++i)
        if(v->graph.edges[i].role==INFS20_MODEL_STRONG) ++strong_count;
    if(strong_count!=v->files_count) return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<v->files_count;++i) {
        const struct mapping *m=&v->files[i];
        int r=node(v,m->root),d=node(v,m->data);
        if(!name_valid(m->name) || !m->ordinal || r<0 || d<0 ||
           v->graph.nodes[r].kind!=INFS20_MODEL_ROOT ||
           v->graph.nodes[r].state!=INFS20_MODEL_LIVE ||
           v->graph.nodes[d].kind!=INFS20_MODEL_DATA)
            return INFS_STATUS_CORRUPT;
        unsigned matches=0;
        for(size_t j=0;j<v->graph.edge_count;++j) {
            const struct infs20_model_edge *e=&v->graph.edges[j];
            if(e->role==INFS20_MODEL_STRONG && e->ordinal==m->ordinal &&
               equal(e->source,m->root) && equal(e->target,m->data)) ++matches;
        }
        if(matches!=1) return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<i;++j)
            if(equal(m->root,v->files[j].root) &&
               (m->ordinal==v->files[j].ordinal || !strcmp(m->name,v->files[j].name)))
                return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}
/* M is the single append frontier; D binds the content digest and size;
 * F keys bind root identity and exact name to a data identity and edge key.
 * All are leaves of the same authenticated tree as N/E ownership records. */
static infs_status decode(void *opaque,const struct infs20_record_view *r)
{
    struct volume *v=opaque;
    if(r->key[0]=='N' || r->key[0]=='E')
        return infs20_model_decode_record(&v->graph,r);
    if(r->key[0]=='M') {
        if(v->header || r->key_size!=1 || r->value_size!=9 || r->value[0]!=1)
            return INFS_STATUS_CORRUPT;
        v->header=1; v->frontier=infs_load_le64(r->value+1);
    } else if(r->key[0]=='D') {
        if(r->key_size!=17 || r->value_size!=37 || r->value[0]!=1 ||
           v->contents_count==INFS20_MODEL_NODES) return INFS_STATUS_CORRUPT;
        struct content *c=&v->contents[v->contents_count++];
        memcpy(c->id,r->key+1,16); c->size=infs_load_le32(r->value+1);
        memcpy(c->digest,r->value+5,32);
    } else if(r->key[0]=='F') {
        if(r->key_size<=17 || r->key_size>17+INFS20_VOLUME_NAME_MAX ||
           r->value_size!=25 || r->value[0]!=1 ||
           v->files_count==INFS20_VOLUME_FILES ||
           memchr(r->key+17,0,r->key_size-17)) return INFS_STATUS_CORRUPT;
        struct mapping *m=&v->files[v->files_count++];
        memcpy(m->root,r->key+1,16); memcpy(m->name,r->key+17,r->key_size-17);
        memcpy(m->data,r->value+1,16); m->ordinal=infs_load_le64(r->value+17);
    } else return INFS_STATUS_NOT_SUPPORTED;
    return INFS_STATUS_OK;
}
static infs_status load(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,struct volume *v)
{
    memset(v,0,sizeof(*v));
    infs_status st=infs20_store_visit(l,io,decode,v);
    return st?st:validate(l,v);
}
static infs_status prepare(void *opaque)
{
    struct pending *p=opaque;
    return p->io->write(p->io->context,p->block,p->data);
}
static infs_status commit(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    const struct volume *v,struct pending *p)
{
    infs_status st=validate(l,v); if(st) return st;
    enum { MAX=1+INFS20_MODEL_NODES+INFS20_VOLUME_FILES };
    struct encoded { uint8_t key[17+INFS20_VOLUME_NAME_MAX],value[37]; } e[MAX];
    struct infs20_store_record r[MAX];
    memset(e,0,sizeof(e));
    size_t n=0;
    e[n].key[0]='M'; e[n].value[0]=1; infs_store_le64(e[n].value+1,v->frontier);
    r[n]=(struct infs20_store_record){e[n].key,e[n].value,1,9}; ++n;
    for(size_t i=0;i<v->contents_count;++i) {
        const struct content *c=&v->contents[i];
        e[n].key[0]='D'; memcpy(e[n].key+1,c->id,16);
        e[n].value[0]=1; infs_store_le32(e[n].value+1,c->size);
        memcpy(e[n].value+5,c->digest,32);
        r[n]=(struct infs20_store_record){e[n].key,e[n].value,17,37}; ++n;
    }
    for(size_t i=0;i<v->files_count;++i) {
        const struct mapping *m=&v->files[i]; size_t len=strlen(m->name);
        e[n].key[0]='F'; memcpy(e[n].key+1,m->root,16);
        memcpy(e[n].key+17,m->name,len); e[n].value[0]=1;
        memcpy(e[n].value+1,m->data,16); infs_store_le64(e[n].value+17,m->ordinal);
        r[n]=(struct infs20_store_record){e[n].key,e[n].value,(uint16_t)(17+len),25}; ++n;
    }
    struct infs20_store_state state;
    return infs20_model_commit_extended(l,io,tx,&v->graph,r,n,
                                         p?prepare:NULL,p,&state);
}
static infs_status reserve(const struct infs20_store_layout *l,
    struct volume *v,uint64_t *block)
{
    /* Frontier only increases. Skip every reserved lane irrespective of its
     * position. Old committed locations never become allocation candidates. */
    uint64_t at=v->frontier;
    for(;;) {
        int moved=0;
        for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
            if(at>=l->lane_start[lane] &&
               at-l->lane_start[lane]<INFS20_STORE_LANE_BLOCKS) {
                at=l->lane_start[lane]+INFS20_STORE_LANE_BLOCKS; moved=1;
            }
        if(!moved) break;
    }
    if(at>=l->member_blocks) return INFS_STATUS_NO_SPACE;
    *block=at; v->frontier=at+1;
    return INFS_STATUS_OK;
}
static infs_status add_file(struct volume *v,const uint8_t root[16],
    const char *name,const uint8_t data[16],uint64_t ordinal)
{
    int d=node(v,data);
    if(d<0) return INFS_STATUS_CORRUPT;
    if(v->files_count==INFS20_VOLUME_FILES || v->graph.edge_count==INFS20_MODEL_EDGES)
        return INFS_STATUS_NO_SPACE;
    struct mapping *m=&v->files[v->files_count++];
    memset(m,0,sizeof(*m)); memcpy(m->root,root,16); memcpy(m->data,data,16);
    memcpy(m->name,name,strlen(name)+1); m->ordinal=ordinal;
    struct infs20_model_edge *e=&v->graph.edges[v->graph.edge_count++];
    memset(e,0,sizeof(*e)); memcpy(e->source,root,16); memcpy(e->target,data,16);
    e->ordinal=ordinal; e->role=INFS20_MODEL_STRONG;
    ++v->graph.nodes[d].refs; v->graph.nodes[d].state=INFS20_MODEL_LIVE;
    return INFS_STATUS_OK;
}
/* Fork the immutable active root. Temporarily pin the old root while copying
 * mappings, then drop only that temporary pin. Snapshots/retired roots remain
 * unchanged. Drain an unpinned old root's outgoing edges in this bounded
 * model; retain zero-reference nodes and never release their physical ranges. */
static infs_status fork_root(const struct infs20_store_layout *l,struct volume *v,
    const uint8_t tx[16],const char *omit,uint8_t fresh[16],uint64_t *ordinal)
{
    int a=pin(v,0);
    uint8_t old[16]; memcpy(old,v->graph.edges[a].target,16);
    id_make(tx,'R',fresh);
    infs_status st=infs20_model_retire_activate(l,&v->graph,fresh,UINT64_MAX);
    if(st) return st;
    size_t original=v->files_count; *ordinal=1;
    for(size_t i=0;i<original;++i) {
        const struct mapping *m=&v->files[i];
        if(equal(m->root,old) && (!omit || strcmp(m->name,omit))) {
            st=add_file(v,fresh,m->name,m->data,(*ordinal)++);
            if(st) return st;
        }
    }
    st=infs20_model_drop_pin(l,&v->graph,UINT64_MAX); if(st) return st;
    int n=node(v,old);
    if(!v->graph.nodes[n].refs) {
        while(v->graph.nodes[n].state!=INFS20_MODEL_RELEASABLE) {
            st=infs20_model_reclaim_step(l,&v->graph,old); if(st) return st;
        }
        size_t kept=0;
        for(size_t i=0;i<v->files_count;++i)
            if(!equal(v->files[i].root,old)) v->files[kept++]=v->files[i];
        v->files_count=kept;
    }
    return INFS_STATUS_OK;
}
infs_status infs20_volume_create(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16])
{
    if(!tx_valid(tx)) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_store_state s;
    infs_status st=infs20_store_recover(l,io,&s);
    if(!st) return INFS_STATUS_ALREADY_EXISTS;
    if(st!=INFS_STATUS_NOT_FOUND) return st;
    struct volume v={0}; v.header=1;
    struct infs20_model_node *n=&v.graph.nodes[v.graph.node_count++];
    id_make(tx,'R',n->id); n->rank=2; n->kind=INFS20_MODEL_ROOT;
    n->state=INFS20_MODEL_LIVE; n->refs=1;
    struct infs20_model_edge *e=&v.graph.edges[v.graph.edge_count++];
    memcpy(e->target,n->id,16); e->ordinal=1; e->role=INFS20_MODEL_ACTIVE;
    return commit(l,io,tx,&v,NULL);
}
infs_status infs20_volume_write(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name,
    const uint8_t *data,size_t size)
{
    if(!tx_valid(tx) || !name_valid(name) || (!data && size))
        return INFS_STATUS_INVALID_ARGUMENT;
    if(size>INFS20_PAGE_BYTES) return INFS_STATUS_FILE_TOO_LARGE;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    uint8_t root[16],id[16]; uint64_t ordinal;
    st=fork_root(l,&v,tx,name,root,&ordinal); if(st) return st;
    if(v.graph.node_count==INFS20_MODEL_NODES || v.contents_count==INFS20_MODEL_NODES)
        return INFS_STATUS_NO_SPACE;
    id_make(tx,'D',id); if(node(&v,id)>=0 || zero(id,16)) return INFS_STATUS_BUSY;
    struct pending p={0}; p.io=io;
    st=reserve(l,&v,&p.block); if(st) return st;
    if(size) memcpy(p.data,data,size);
    struct infs20_model_node *n=&v.graph.nodes[v.graph.node_count++];
    memset(n,0,sizeof(*n)); memcpy(n->id,id,16); n->kind=INFS20_MODEL_DATA;
    n->start=p.block; n->blocks=1; n->state=INFS20_MODEL_PENDING;
    struct content *c=&v.contents[v.contents_count++];
    memcpy(c->id,id,16); c->size=(uint32_t)size; infs_sha256(p.data,sizeof(p.data),c->digest);
    st=add_file(&v,root,name,id,ordinal);
    return st?st:commit(l,io,tx,&v,&p);
}
infs_status infs20_volume_clone(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *source,const char *target)
{
    if(!tx_valid(tx) || !name_valid(source) || !name_valid(target) || !strcmp(source,target))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    const uint8_t *active=v.graph.edges[pin(&v,0)].target;
    int i=file(&v,active,source);
    if(i<0) return INFS_STATUS_NOT_FOUND;
    if(file(&v,active,target)>=0) return INFS_STATUS_ALREADY_EXISTS;
    uint8_t data[16],root[16]; uint64_t ordinal;
    memcpy(data,v.files[i].data,16);
    st=fork_root(l,&v,tx,NULL,root,&ordinal); if(st) return st;
    st=add_file(&v,root,target,data,ordinal);
    return st?st:commit(l,io,tx,&v,NULL);
}
static infs_status read_content(const struct volume *v,const struct infs20_store_io *io,
    const uint8_t id[16],uint8_t page[INFS20_PAGE_BYTES],size_t *size)
{
    int d=node(v,id),c=content(v,id);
    if(d<0 || c<0) return INFS_STATUS_CORRUPT;
    infs_status st=io->read(io->context,v->graph.nodes[d].start,page); if(st) return st;
    uint8_t digest[32]; infs_sha256(page,INFS20_PAGE_BYTES,digest);
    *size=v->contents[c].size;
    if(memcmp(digest,v->contents[c].digest,32) || !zero(page+*size,INFS20_PAGE_BYTES-*size))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}
infs_status infs20_volume_read(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,uint64_t ordinal,const char *name,
    uint8_t *out,size_t capacity,size_t *size)
{
    if(!name_valid(name) || !size || (!out && capacity)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    int p=pin(&v,ordinal); if(p<0) return INFS_STATUS_NOT_FOUND;
    int i=file(&v,v.graph.edges[p].target,name); if(i<0) return INFS_STATUS_NOT_FOUND;
    uint8_t page[INFS20_PAGE_BYTES]; size_t n=0;
    st=read_content(&v,io,v.files[i].data,page,&n); if(st) return st;
    if(n>capacity) return INFS_STATUS_NO_SPACE;
    if(n) memcpy(out,page,n);
    *size=n; return INFS_STATUS_OK;
}
infs_status infs20_volume_snapshot(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    if(pin(&v,ordinal)>=0) return INFS_STATUS_ALREADY_EXISTS;
    if(v.graph.edge_count==INFS20_MODEL_EDGES) return INFS_STATUS_NO_SPACE;
    struct infs20_model_edge *e=&v.graph.edges[v.graph.edge_count++];
    memset(e,0,sizeof(*e)); memcpy(e->target,v.graph.edges[pin(&v,0)].target,16);
    e->role=INFS20_MODEL_SNAPSHOT; e->ordinal=ordinal;
    ++v.graph.nodes[node(&v,e->target)].refs;
    return commit(l,io,tx,&v,NULL);
}
infs_status infs20_volume_format(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    uint8_t root[16]; id_make(tx,'R',root);
    st=infs20_model_retire_activate(l,&v.graph,root,ordinal);
    return st?st:commit(l,io,tx,&v,NULL);
}
infs_status infs20_volume_unformat(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    int p=pin(&v,ordinal); if(p<0 || v.graph.edges[p].role!=INFS20_MODEL_RETIRED)
        return INFS_STATUS_NOT_FOUND;
    /* Verify all payloads that will become visible before changing authority. */
    for(size_t i=0;i<v.files_count;++i)
        if(equal(v.files[i].root,v.graph.edges[p].target)) {
            uint8_t page[INFS20_PAGE_BYTES]; size_t n;
            st=read_content(&v,io,v.files[i].data,page,&n); if(st) return st;
        }
    st=infs20_model_reactivate(l,&v.graph,ordinal);
    return st?st:commit(l,io,tx,&v,NULL);
}
infs_status infs20_volume_relocate(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name)
{
    if(!tx_valid(tx) || !name_valid(name)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,io,&v); if(st) return st;
    int i=file(&v,v.graph.edges[pin(&v,0)].target,name);
    if(i<0) return INFS_STATUS_NOT_FOUND;
    struct pending p={0}; p.io=io; size_t size;
    st=read_content(&v,io,v.files[i].data,p.data,&size); if(st) return st;
    st=reserve(l,&v,&p.block); if(st) return st;
    v.graph.nodes[node(&v,v.files[i].data)].start=p.block;
    return commit(l,io,tx,&v,&p);
}
