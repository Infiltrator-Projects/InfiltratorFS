// SPDX-License-Identifier: GPL-3.0-or-later
#include "volume.h"
#include "infilfs/checksum.h"
#include "infilfs/endian.h"
#include <string.h>
#include <stdlib.h>

/*
 * Every name mapping is an owning edge from an immutable namespace root to
 * a content allocation. Its descriptor owns the logical length and digest;
 * the graph's node range is the sole physical location authority. Reflinks
 * share both. Retired physical ranges transfer into the canonical tree's
 * quarantine ledger until a later fully protected commit replaces the bank
 * that could still reference them. Exclusive access is required throughout.
 * This is bounded payload reuse, not production metadata-arena collection.
 */
struct mapping {
    uint8_t root[16], data[16];
    char name[INFS20_VOLUME_NAME_MAX+1];
    uint64_t ordinal;
};
struct content {
    uint8_t id[16], digest[INFS20_VOLUME_UNITS][32];
    uint32_t size;
};
enum { QUARANTINE_MAX=32, QUARANTINE_VALUE=1+24*QUARANTINE_MAX };
struct quarantine { uint64_t start,blocks,sequence; };
struct volume {
    struct infs20_model_graph graph;
    struct mapping files[INFS20_VOLUME_FILES];
    struct content contents[INFS20_MODEL_NODES];
    size_t files_count, contents_count;
    uint64_t frontier,sequence;
    struct quarantine quarantine[QUARANTINE_MAX];
    size_t quarantine_count;
    unsigned header,fully_protected;
};
struct pending {
    const struct infs20_store_io *io;
    uint64_t block;
    size_t blocks;
    uint8_t data[INFS20_VOLUME_BYTES];
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
/* A zero-length file still owns one authenticated zero block in this model.
 * Production inline/empty object representation is a separate typed case.
 * Bounds on the caller's size are established before deriving unit count. */
static size_t unit_count(size_t size)
{
    return size ? 1u+(size-1u)/INFS20_PAGE_BYTES : 1u;
}
static size_t unit_size(size_t size,size_t index)
{
    size_t offset=index*INFS20_PAGE_BYTES;
    if(offset>=size) return 0;
    size_t left=size-offset;
    return left<INFS20_PAGE_BYTES?left:INFS20_PAGE_BYTES;
}
static int overlaps(uint64_t a,uint64_t n,uint64_t b,uint64_t m)
{
    /* All range ends are bounded by the independently supplied geometry. */
    return n && m && a<b+m && b<a+n;
}
static infs_status validate(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct volume *v)
{
    infs_status st=infs20_model_validate(l,&v->graph);
    if(st) return st;
    if(v->header!=1 || v->frontier>l->member_blocks ||
       v->files_count>INFS20_VOLUME_FILES || v->contents_count>INFS20_MODEL_NODES)
        return INFS_STATUS_CORRUPT;
    if(paged) for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
        if(!paged->arena_blocks[lane] || paged->arena_start[lane]>=l->member_blocks ||
           paged->arena_blocks[lane]>l->member_blocks-paged->arena_start[lane])
            return INFS_STATUS_CORRUPT;
    size_t data_count=0,strong_count=0;
    for(size_t i=0;i<v->graph.node_count;++i) {
        const struct infs20_model_node *n=&v->graph.nodes[i];
        if(n->kind==INFS20_MODEL_DATA) {
            ++data_count;
            int c=content(v,n->id);
            if(c<0 || v->contents[c].size>INFS20_VOLUME_BYTES ||
               n->blocks!=unit_count(v->contents[c].size) ||
               n->start>=v->frontier || n->blocks>v->frontier-n->start)
                return INFS_STATUS_CORRUPT;
            /* Metadata arenas are reserved in their entirety, including
             * unused append capacity. No payload may overlap any arena. */
            if(paged) for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
                if(n->start<paged->arena_start[lane]+paged->arena_blocks[lane] &&
                   paged->arena_start[lane]<n->start+n->blocks)
                    return INFS_STATUS_CORRUPT;
        } else if(n->kind!=INFS20_MODEL_ROOT || n->blocks)
            return INFS_STATUS_CORRUPT;
    }
    if(data_count!=v->contents_count || v->quarantine_count>QUARANTINE_MAX)
        return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<v->quarantine_count;++i) {
        const struct quarantine *q=&v->quarantine[i];
        if(!q->sequence || !q->blocks || q->start>=v->frontier ||
           q->blocks>v->frontier-q->start ||
           (i && v->quarantine[i-1].start+v->quarantine[i-1].blocks>q->start))
            return INFS_STATUS_CORRUPT;
        for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
            if(overlaps(q->start,q->blocks,l->lane_start[lane],INFS20_STORE_LANE_BLOCKS) ||
               (paged && overlaps(q->start,q->blocks,paged->arena_start[lane],
                                  paged->arena_blocks[lane])))
                return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<v->graph.node_count;++j)
            if(overlaps(q->start,q->blocks,v->graph.nodes[j].start,
                        v->graph.nodes[j].blocks)) return INFS_STATUS_CORRUPT;
    }
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
        if(v->header || r->key_size!=1 || r->value_size!=9)
            return INFS_STATUS_CORRUPT;
        /* Old append-only media cannot prove former relocation ranges.
         * Refuse the obsolete profile rather than infer safe free space. */
        if(r->value[0]!=2) return INFS_STATUS_NOT_SUPPORTED;
        v->header=1; v->frontier=infs_load_le64(r->value+1);
    } else if(r->key[0]=='Q') {
        /* Single ordered range ledger; independent of deleted object IDs. */
        if(v->quarantine_count || r->key_size!=1 || r->value_size<25 ||
           r->value[0]!=1 || (r->value_size-1)%24 ||
           (r->value_size-1)/24>QUARANTINE_MAX) return INFS_STATUS_CORRUPT;
        v->quarantine_count=(r->value_size-1)/24;
        for(size_t i=0;i<v->quarantine_count;++i) {
            const uint8_t *p=r->value+1+24*i;
            v->quarantine[i]=(struct quarantine){
                infs_load_le64(p),infs_load_le64(p+8),infs_load_le64(p+16)};
        }
    } else if(r->key[0]=='D') {
        /* D version 2 stores exactly one indexed digest per logical unit.
         * This is a bounded record, not the production checksum tree.
         * Probe media have no backward-format compatibility obligation. */
        if(r->key_size!=17 || r->value_size<5 ||
           r->value[0]!=2 || v->contents_count==INFS20_MODEL_NODES)
            return INFS_STATUS_CORRUPT;
        uint32_t size=infs_load_le32(r->value+1);
        if(size>INFS20_VOLUME_BYTES || r->value_size!=5+32*unit_count(size))
            return INFS_STATUS_CORRUPT;
        struct content *c=&v->contents[v->contents_count++];
        memcpy(c->id,r->key+1,16); c->size=size;
        memcpy(c->digest,r->value+5,32*unit_count(size));
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
static infs_status decode_paged(void *opaque,const uint8_t *key,size_t ks,
    const uint8_t *value,size_t vs)
{
    const struct infs20_record_view r={INFS20_PROBE_KIND,key,ks,value,vs,0};
    return decode(opaque,&r);
}
static infs_status load(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,struct volume *v)
{
    memset(v,0,sizeof(*v));
    /* Both reads select the same authority under exclusive access. */
    struct infs20_store_state authority;
    infs_status st;
    if(paged) {
        struct infs20_paged_state state;
        st=infs20_paged_open(paged,io,&state);
        if(st) return st;
        authority=state.commit;
        st=infs20_paged_visit(paged,io,decode_paged,v);
    } else {
        st=infs20_store_recover(l,io,&authority);
        if(st) return st;
        st=infs20_store_visit(l,io,decode,v);
    }
    if(st) return st;
    v->sequence=authority.sequence;
    v->fully_protected=authority.fully_protected;
    for(size_t i=0;i<v->quarantine_count;++i)
        if(v->quarantine[i].sequence>v->sequence) return INFS_STATUS_CORRUPT;
    return validate(l,paged,v);
}
static infs_status prepare(void *opaque)
{
    struct pending *p=opaque;
    /* All private payload units participate in the store dependency flush.
     * An error after any unit leaves the old committed mapping authoritative. */
    for(size_t i=0;i<p->blocks;++i) {
        infs_status st=p->io->write(p->io->context,p->block+i,
                                    p->data+i*INFS20_PAGE_BYTES);
        if(st) return st;
    }
    return INFS_STATUS_OK;
}
/* Compare complete stored units, including deterministic tail padding, with
 * the same immutable staging buffer used to construct the allocation digest.
 * Run only after the common dependency flush, before any root is published. */
static infs_status verify_payload(void *opaque)
{
    const struct pending *p=opaque;
    uint8_t page[INFS20_PAGE_BYTES];
    for(size_t i=0;i<p->blocks;++i) {
        infs_status st=p->io->read(p->io->context,p->block+i,page);
        if(st) return st;
        if(memcmp(page,p->data+i*INFS20_PAGE_BYTES,sizeof(page)))
            return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}
struct paged_publish {
    const struct infs20_paged_layout *layout;
    const struct infs20_store_io *io;
    const uint8_t *tx;
    struct pending *payload;
    int create;
};
static infs_status publish_paged(void *opaque,const struct infs20_store_record *r,size_t n)
{
    struct paged_publish *p=opaque;
    struct infs20_paged_state state;
    return infs20_paged_replace(p->layout,p->io,p->tx,r,n,p->create,
        p->payload?prepare:NULL,p->payload?verify_payload:NULL,p->payload,&state);
}
static infs_status commit(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,
    const uint8_t tx[16],const struct volume *v,struct pending *p,int create)
{
    infs_status st=validate(l,paged,v); if(st) return st;
    if(v->sequence==UINT64_MAX) return INFS_STATUS_OVERFLOW;
    for(size_t i=0;i<v->quarantine_count;++i)
        if(v->quarantine[i].sequence>v->sequence+1) return INFS_STATUS_CORRUPT;
    enum { MAX=2+INFS20_MODEL_NODES+INFS20_VOLUME_FILES,
           VALUE_MAX=QUARANTINE_VALUE>5+32*INFS20_VOLUME_UNITS?
                     QUARANTINE_VALUE:5+32*INFS20_VOLUME_UNITS };
    struct encoded {
        uint8_t key[17+INFS20_VOLUME_NAME_MAX],value[VALUE_MAX];
    } e[MAX];
    struct infs20_store_record r[MAX];
    memset(e,0,sizeof(e));
    size_t n=0;
    e[n].key[0]='M'; e[n].value[0]=2; infs_store_le64(e[n].value+1,v->frontier);
    r[n]=(struct infs20_store_record){e[n].key,e[n].value,1,9}; ++n;
    if(v->quarantine_count) {
        e[n].key[0]='Q'; e[n].value[0]=1;
        for(size_t i=0;i<v->quarantine_count;++i) {
            uint8_t *p=e[n].value+1+24*i;
            infs_store_le64(p,v->quarantine[i].start);
            infs_store_le64(p+8,v->quarantine[i].blocks);
            infs_store_le64(p+16,v->quarantine[i].sequence);
        }
        r[n]=(struct infs20_store_record){e[n].key,e[n].value,1,
            (uint16_t)(1+24*v->quarantine_count)}; ++n;
    }
    for(size_t i=0;i<v->contents_count;++i) {
        const struct content *c=&v->contents[i];
        e[n].key[0]='D'; memcpy(e[n].key+1,c->id,16);
        size_t units=unit_count(c->size);
        e[n].value[0]=2; infs_store_le32(e[n].value+1,c->size);
        memcpy(e[n].value+5,c->digest,32*units);
        r[n]=(struct infs20_store_record){e[n].key,e[n].value,17,
                                          (uint16_t)(5+32*units)}; ++n;
    }
    for(size_t i=0;i<v->files_count;++i) {
        const struct mapping *m=&v->files[i]; size_t len=strlen(m->name);
        e[n].key[0]='F'; memcpy(e[n].key+1,m->root,16);
        memcpy(e[n].key+17,m->name,len); e[n].value[0]=1;
        memcpy(e[n].value+1,m->data,16); infs_store_le64(e[n].value+17,m->ordinal);
        r[n]=(struct infs20_store_record){e[n].key,e[n].value,(uint16_t)(17+len),25}; ++n;
    }
    if(paged) {
        struct paged_publish publish={paged,io,tx,p,create};
        return infs20_model_with_records(l,&v->graph,r,n,publish_paged,&publish);
    }
    struct infs20_store_state state;
    return infs20_model_commit_extended(l,io,tx,&v->graph,r,n,
                                         p?prepare:NULL,p?verify_payload:NULL,p,&state);
}
/*
 * At retirement commit R the other bank still contains R-1. Only after
 * a fully protected R+1 may those payload blocks be reused. A surviving
 * degraded authority remains readable but cannot authorize reuse.
 */
static void prune_quarantine(struct volume *v)
{
    if(!v->fully_protected) return;
    size_t n=0;
    for(size_t i=0;i<v->quarantine_count;++i)
        if(v->quarantine[i].sequence>=v->sequence)
            v->quarantine[n++]=v->quarantine[i];
    v->quarantine_count=n;
}
static infs_status quarantine_range(struct volume *v,uint64_t start,uint64_t blocks)
{
    if(!blocks) return INFS_STATUS_OK;
    if(!v->fully_protected) return INFS_STATUS_BUSY;
    if(v->sequence==UINT64_MAX) return INFS_STATUS_OVERFLOW;
    prune_quarantine(v);
    if(v->quarantine_count==QUARANTINE_MAX) return INFS_STATUS_NO_SPACE;
    size_t i=0;
    while(i<v->quarantine_count && v->quarantine[i].start<start) ++i;
    memmove(v->quarantine+i+1,v->quarantine+i,
            (v->quarantine_count-i)*sizeof(v->quarantine[0]));
    v->quarantine[i]=(struct quarantine){start,blocks,v->sequence+1};
    ++v->quarantine_count;
    return INFS_STATUS_OK;
}
static void skip_range(uint64_t at,size_t blocks,uint64_t start,uint64_t count,
                       uint64_t *next)
{
    if(overlaps(at,blocks,start,count) && start+count>*next) *next=start+count;
}
static infs_status find_run(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct volume *v,
    uint64_t at,size_t blocks,uint64_t *block)
{
    for(;;) {
        if(at>=l->member_blocks || blocks>l->member_blocks-at)
            return INFS_STATUS_NO_SPACE;
        uint64_t next=at;
        for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
            skip_range(at,blocks,l->lane_start[lane],INFS20_STORE_LANE_BLOCKS,&next);
            if(paged) skip_range(at,blocks,paged->arena_start[lane],
                                 paged->arena_blocks[lane],&next);
        }
        for(size_t i=0;i<v->graph.node_count;++i)
            skip_range(at,blocks,v->graph.nodes[i].start,v->graph.nodes[i].blocks,&next);
        for(size_t i=0;i<v->quarantine_count;++i)
            skip_range(at,blocks,v->quarantine[i].start,v->quarantine[i].blocks,&next);
        if(next==at) { *block=at; return INFS_STATUS_OK; }
        at=next;
    }
}
static infs_status reserve(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,struct volume *v,size_t blocks,uint64_t *block)
{
    if(!blocks || blocks>INFS20_VOLUME_UNITS) return INFS_STATUS_INVALID_ARGUMENT;
    if(!v->fully_protected) return INFS_STATUS_BUSY;
    prune_quarantine(v);
    /* Preserve append locality; on exhaustion exclude every current owner
     * and unretired range. The bounded scan is not a production free index. */
    infs_status st=find_run(l,paged,v,v->frontier,blocks,block);
    if(st==INFS_STATUS_NO_SPACE) st=find_run(l,paged,v,0,blocks,block);
    if(!st && *block+blocks>v->frontier) v->frontier=*block+blocks;
    return st;
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
 * model; collection transfers zero-reference physical owners into quarantine. */
static infs_status fork_root(const struct infs20_store_layout *l,struct volume *v,
    const uint8_t tx[16],const char *omit,const char *also_omit,
    uint8_t fresh[16],uint64_t *ordinal)
{
    int a=pin(v,0);
    uint8_t old[16]; memcpy(old,v->graph.edges[a].target,16);
    id_make(tx,'R',fresh);
    infs_status st=infs20_model_retire_activate(l,&v->graph,fresh,UINT64_MAX);
    if(st) return st;
    size_t original=v->files_count; *ordinal=1;
    for(size_t i=0;i<original;++i) {
        const struct mapping *m=&v->files[i];
        if(equal(m->root,old) && (!omit || strcmp(m->name,omit)) &&
           (!also_omit || strcmp(m->name,also_omit))) {
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
static infs_status volume_create(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,
    const uint8_t tx[16],uint64_t preferred_start)
{
    if(!l || !io || !tx_valid(tx) || preferred_start>=l->member_blocks)
        return INFS_STATUS_INVALID_ARGUMENT;
    if(!paged) {
        struct infs20_store_state s;
        infs_status st=infs20_store_recover(l,io,&s);
        if(!st) return INFS_STATUS_ALREADY_EXISTS;
        if(st!=INFS_STATUS_NOT_FOUND) return st;
    }
    struct volume v={0}; v.header=1;
    /* This is a persisted allocation preference, never a free-space claim.
     * The existing checked allocator skips all reserved and owned ranges and
     * can wrap on exhaustion. It supports comparable formatter experiments
     * without hard-coding an unqualified production placement policy. */
    v.frontier=preferred_start;
    struct infs20_model_node *n=&v.graph.nodes[v.graph.node_count++];
    id_make(tx,'R',n->id); n->rank=2; n->kind=INFS20_MODEL_ROOT;
    n->state=INFS20_MODEL_LIVE; n->refs=1;
    struct infs20_model_edge *e=&v.graph.edges[v.graph.edge_count++];
    memcpy(e->target,n->id,16); e->ordinal=1; e->role=INFS20_MODEL_ACTIVE;
    return commit(l,paged,io,tx,&v,NULL,1);
}
static infs_status volume_write(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,const uint8_t tx[16],const char *name,
    const uint8_t *data,size_t size)
{
    if(!tx_valid(tx) || !name_valid(name) || (!data && size))
        return INFS_STATUS_INVALID_ARGUMENT;
    if(size>INFS20_VOLUME_BYTES) return INFS_STATUS_FILE_TOO_LARGE;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    uint8_t root[16],id[16]; uint64_t ordinal;
    st=fork_root(l,&v,tx,name,NULL,root,&ordinal); if(st) return st;
    if(v.graph.node_count==INFS20_MODEL_NODES || v.contents_count==INFS20_MODEL_NODES)
        return INFS_STATUS_NO_SPACE;
    id_make(tx,'D',id); if(node(&v,id)>=0 || zero(id,16)) return INFS_STATUS_BUSY;
    struct pending *p=calloc(1,sizeof(*p));
    if(!p) return INFS_STATUS_NO_MEMORY;
    p->io=io; p->blocks=unit_count(size);
    st=reserve(l,paged,&v,p->blocks,&p->block);
    if(st) { free(p); return st; }
    if(size) memcpy(p->data,data,size);
    struct infs20_model_node *n=&v.graph.nodes[v.graph.node_count++];
    memset(n,0,sizeof(*n)); memcpy(n->id,id,16); n->kind=INFS20_MODEL_DATA;
    n->start=p->block; n->blocks=p->blocks; n->state=INFS20_MODEL_PENDING;
    struct content *c=&v.contents[v.contents_count++];
    memset(c,0,sizeof(*c)); memcpy(c->id,id,16); c->size=(uint32_t)size;
    for(size_t i=0;i<p->blocks;++i)
        infs_sha256(p->data+i*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES,c->digest[i]);
    st=add_file(&v,root,name,id,ordinal);
    if(!st) st=commit(l,paged,io,tx,&v,p,0);
    free(p);
    return st;
}
static infs_status volume_clone(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,const uint8_t tx[16],const char *source,const char *target)
{
    if(!tx_valid(tx) || !name_valid(source) || !name_valid(target) || !strcmp(source,target))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    const uint8_t *active=v.graph.edges[pin(&v,0)].target;
    int i=file(&v,active,source);
    if(i<0) return INFS_STATUS_NOT_FOUND;
    if(file(&v,active,target)>=0) return INFS_STATUS_ALREADY_EXISTS;
    uint8_t data[16],root[16]; uint64_t ordinal;
    memcpy(data,v.files[i].data,16);
    st=fork_root(l,&v,tx,NULL,NULL,root,&ordinal); if(st) return st;
    st=add_file(&v,root,target,data,ordinal);
    return st?st:commit(l,paged,io,tx,&v,NULL,0);
}
/* The canonical allocation digest is indexed by decoded unit. Verify padding
 * explicitly, even though the digest also covers it, to avoid accepting a
 * semantically noncanonical last unit from a malformed committed descriptor. */
static infs_status read_unit(const struct volume *v,const struct infs20_store_io *io,
    const uint8_t id[16],size_t index,uint8_t page[INFS20_PAGE_BYTES])
{
    int d=node(v,id),c=content(v,id);
    if(d<0 || c<0 || index>=unit_count(v->contents[c].size))
        return INFS_STATUS_CORRUPT;
    infs_status st=io->read(io->context,v->graph.nodes[d].start+index,page);
    if(st) return st;
    uint8_t digest[32]; infs_sha256(page,INFS20_PAGE_BYTES,digest);
    size_t size=unit_size(v->contents[c].size,index);
    if(memcmp(digest,v->contents[c].digest[index],32) ||
       !zero(page+size,INFS20_PAGE_BYTES-size))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}
/* Caller-owned private buffer is not published until every unit verifies.
 * With NULL, validate the allocation without assembling its contents. */
static infs_status read_content(const struct volume *v,const struct infs20_store_io *io,
    const uint8_t id[16],uint8_t *private_data)
{
    int c=content(v,id);
    if(c<0) return INFS_STATUS_CORRUPT;
    uint8_t page[INFS20_PAGE_BYTES];
    for(size_t i=0;i<unit_count(v->contents[c].size);++i) {
        infs_status st=read_unit(v,io,id,i,page);
        if(st) return st;
        if(private_data) memcpy(private_data+i*INFS20_PAGE_BYTES,page,sizeof(page));
    }
    return INFS_STATUS_OK;
}
static infs_status volume_read(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,uint64_t ordinal,const char *name,
    uint8_t *out,size_t capacity,size_t *size)
{
    if(!name_valid(name) || !size || (!out && capacity)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    int p=pin(&v,ordinal); if(p<0) return INFS_STATUS_NOT_FOUND;
    int i=file(&v,v.graph.edges[p].target,name); if(i<0) return INFS_STATUS_NOT_FOUND;
    int ci=content(&v,v.files[i].data);
    if(ci<0) return INFS_STATUS_CORRUPT;
    size_t n=v.contents[ci].size;
    if(n>capacity) return INFS_STATUS_NO_SPACE;
    uint8_t *stage=malloc(unit_count(n)*INFS20_PAGE_BYTES);
    if(!stage) return INFS_STATUS_NO_MEMORY;
    st=read_content(&v,io,v.files[i].data,stage);
    if(!st) {
        if(n) memcpy(out,stage,n);
        *size=n;
    }
    free(stage);
    return st;
}
static infs_status volume_read_range(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,uint64_t ordinal,const char *name,
    uint64_t offset,uint8_t *out,size_t capacity,size_t *size)
{
    if(!name_valid(name) || !size || (!out && capacity)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    int p=pin(&v,ordinal); if(p<0) return INFS_STATUS_NOT_FOUND;
    int i=file(&v,v.graph.edges[p].target,name); if(i<0) return INFS_STATUS_NOT_FOUND;
    int ci=content(&v,v.files[i].data);
    if(ci<0) return INFS_STATUS_CORRUPT;
    size_t total=v.contents[ci].size;
    if(offset>=total || !capacity) { *size=0; return INFS_STATUS_OK; }
    size_t from=(size_t)offset,n=total-from;
    if(n>capacity) n=capacity;
    uint8_t *stage=malloc(n);
    if(!stage) return INFS_STATUS_NO_MEMORY;
    uint8_t page[INFS20_PAGE_BYTES];
    size_t done=0;
    while(done<n) {
        size_t at=from+done,index=at/INFS20_PAGE_BYTES,skip=at%INFS20_PAGE_BYTES;
        size_t take=INFS20_PAGE_BYTES-skip;
        if(take>n-done) take=n-done;
        st=read_unit(&v,io,v.files[i].data,index,page);
        if(st) break;
        memcpy(stage+done,page+skip,take); done+=take;
    }
    if(!st) { memcpy(out,stage,n); *size=n; }
    free(stage);
    return st;
}
/*
 * Indexed read-only resolver. The old bounded full-graph loader is still
 * the independent mutation/recovery oracle. Ordinary reads authenticate
 * only the pin, root, name, strong edge, allocation node, checksum record,
 * geometry header and quarantine ledger they actually consume. A global
 * cross-allocation ownership/refcount audit is explicitly not implied.
 *
 * One selected immutable root is borrowed for the whole read; the caller's
 * exclusive immutable access also pins physical placement. Reuse the
 * existing canonical typed decoders and read_unit() digest implementation.
 */
struct indexed_read {
    const struct infs20_paged_layout *layout;
    const struct infs20_store_io *io;
    uint64_t ordinal,offset;
    const char *name;
    uint8_t *out;
    size_t capacity,*size;
    int whole;
};
static infs_status get_record(const struct infs20_tree_io *tree,
    const struct infs20_tree_ref *root,const uint8_t *key,size_t key_size,
    struct volume *v)
{
    uint8_t value[INFS20_PAYLOAD_BYTES]; size_t n=0;
    infs_status st=infs20_tree_get(tree,root,key,key_size,value,sizeof(value),&n);
    if(st) return st;
    return decode_paged(v,key,key_size,value,n);
}
static infs_status required_record(const struct infs20_tree_io *tree,
    const struct infs20_tree_ref *root,const uint8_t *key,size_t key_size,
    struct volume *v)
{
    infs_status st=get_record(tree,root,key,key_size,v);
    return st==INFS_STATUS_NOT_FOUND?INFS_STATUS_CORRUPT:st;
}
struct active_lookup { struct volume *v; unsigned found; };
static infs_status active_visit(void *opaque,const uint8_t *key,size_t ks,
    const uint8_t *value,size_t vs)
{
    struct active_lookup *a=opaque;
    /* The zero-source E prefix is reserved for root pins. Decode each
     * locally without retaining an array of unrelated graph edges. */
    if(ks!=25 || key[0]!='E' || !zero(key+1,16) ||
       vs!=24 || value[0]!=1 || !zero(value+2,6) ||
       !infs_load_le64(key+17) || zero(value+8,16) ||
       value[1]<INFS20_MODEL_ACTIVE || value[1]>INFS20_MODEL_OPERATION)
        return INFS_STATUS_CORRUPT;
    if(value[1]!=INFS20_MODEL_ACTIVE) return INFS_STATUS_OK;
    if(a->found++) return INFS_STATUS_CORRUPT;
    return decode_paged(a->v,key,ks,value,vs);
}
static infs_status indexed_resolve(struct indexed_read *q,
    const struct infs20_tree_io *tree,const struct infs20_tree_ref *root,
    uint64_t sequence,struct volume *v)
{
    uint8_t key[17+INFS20_VOLUME_NAME_MAX]={0};
    key[0]='E';
    infs_status st;
    if(q->ordinal) {
        infs_store_le64(key+17,q->ordinal);
        st=get_record(tree,root,key,25,v);
        if(st) return st;
    } else {
        /* Exact lexicographic prefix interval E || zero-source. */
        uint8_t upper[17]={0}; upper[0]='E'; upper[16]=1;
        struct active_lookup a={v,0};
        size_t seen=0; int more=0;
        st=infs20_tree_visit_range(tree,root,key,17,0,upper,sizeof(upper),
            INFS20_MODEL_EDGES,active_visit,&a,&seen,&more);
        if(st) return st;
        if(more || a.found!=1) return INFS_STATUS_CORRUPT;
    }
    const struct infs20_model_edge *p=&v->graph.edges[0];
    if(v->graph.edge_count!=1 || !zero(p->source,16) ||
       !p->ordinal || zero(p->target,16) ||
       p->role<INFS20_MODEL_ACTIVE || p->role>INFS20_MODEL_OPERATION)
        return INFS_STATUS_CORRUPT;
    uint8_t root_id[16]; memcpy(root_id,p->target,16);
    key[0]='N'; memcpy(key+1,root_id,16);
    st=required_record(tree,root,key,17,v); if(st) return st;
    const struct infs20_model_node *rn=&v->graph.nodes[0];
    if(v->graph.node_count!=1 || rn->kind!=INFS20_MODEL_ROOT ||
       rn->state!=INFS20_MODEL_LIVE || !rn->refs || !rn->rank ||
       rn->start || rn->blocks) return INFS_STATUS_CORRUPT;
    key[0]='F'; size_t n=strlen(q->name);
    memcpy(key+17,q->name,n);
    st=get_record(tree,root,key,17+n,v); if(st) return st;
    const struct mapping *m=&v->files[0];
    if(v->files_count!=1 || !name_valid(m->name) || !m->ordinal ||
       zero(m->data,16) || !equal(m->root,root_id) ||
       strcmp(m->name,q->name)) return INFS_STATUS_CORRUPT;
    key[0]='E'; infs_store_le64(key+17,m->ordinal);
    st=required_record(tree,root,key,25,v); if(st) return st;
    const struct infs20_model_edge *edge=&v->graph.edges[1];
    if(v->graph.edge_count!=2 || edge->role!=INFS20_MODEL_STRONG ||
       edge->ordinal!=m->ordinal || !equal(edge->source,root_id) ||
       !equal(edge->target,m->data)) return INFS_STATUS_CORRUPT;
    key[0]='N'; memcpy(key+1,m->data,16);
    st=required_record(tree,root,key,17,v); if(st) return st;
    key[0]='D'; st=required_record(tree,root,key,17,v); if(st) return st;
    key[0]='M'; st=required_record(tree,root,key,1,v); if(st) return st;
    const struct infs20_model_node *d=&v->graph.nodes[1];
    const struct content *c=&v->contents[0];
    const struct infs20_store_layout *l=&q->layout->bootstrap;
    if(v->graph.node_count!=2 || v->contents_count!=1 || !v->header ||
       d->kind!=INFS20_MODEL_DATA || d->state!=INFS20_MODEL_LIVE ||
       !d->refs || d->rank || !equal(d->id,c->id) ||
       d->blocks!=unit_count(c->size) || v->frontier>l->member_blocks ||
       d->start>=v->frontier || d->blocks>v->frontier-d->start)
        return INFS_STATUS_CORRUPT;
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
        if(overlaps(d->start,d->blocks,l->lane_start[lane],INFS20_STORE_LANE_BLOCKS) ||
           overlaps(d->start,d->blocks,q->layout->arena_start[lane],
                    q->layout->arena_blocks[lane]))
            return INFS_STATUS_CORRUPT;
    key[0]='Q'; st=get_record(tree,root,key,1,v);
    if(st && st!=INFS_STATUS_NOT_FOUND) return st;
    for(size_t i=0;i<v->quarantine_count;++i) {
        const struct quarantine *r=&v->quarantine[i];
        if(!r->sequence || r->sequence>sequence || !r->blocks ||
           r->start>=v->frontier || r->blocks>v->frontier-r->start ||
           (i && v->quarantine[i-1].start+v->quarantine[i-1].blocks>r->start) ||
           overlaps(r->start,r->blocks,d->start,d->blocks))
            return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}
static infs_status indexed_read_view(void *opaque,const struct infs20_tree_io *tree,
    const struct infs20_tree_ref *root,uint64_t sequence)
{
    struct indexed_read *q=opaque;
    struct volume *v=calloc(1,sizeof(*v));
    if(!v) return INFS_STATUS_NO_MEMORY;
    infs_status st=indexed_resolve(q,tree,root,sequence,v);
    if(st) { free(v); return st; }
    size_t total=v->contents[0].size;
    if(q->whole && total>q->capacity) { free(v); return INFS_STATUS_NO_SPACE; }
    if(!q->whole && (q->offset>=total || !q->capacity)) {
        *q->size=0; free(v); return INFS_STATUS_OK;
    }
    /* Whole-file reads verify the zero block even for an empty file,
     * matching the previous read contract. Range reads verify only
     * intersecting logical units, and never publish partial output. */
    size_t from=q->whole?0:(size_t)q->offset;
    size_t n=total-from; if(n>q->capacity) n=q->capacity;
    uint8_t *stage=malloc(q->whole?unit_count(total)*INFS20_PAGE_BYTES:n);
    if(!stage) { free(v); return INFS_STATUS_NO_MEMORY; }
    if(q->whole) {
        st=read_content(v,q->io,v->files[0].data,stage);
    } else {
        uint8_t page[INFS20_PAGE_BYTES]; size_t done=0;
        while(done<n) {
            size_t at=from+done,index=at/INFS20_PAGE_BYTES,skip=at%INFS20_PAGE_BYTES;
            size_t take=INFS20_PAGE_BYTES-skip;
            if(take>n-done) take=n-done;
            st=read_unit(v,q->io,v->files[0].data,index,page);
            if(st) break;
            memcpy(stage+done,page+skip,take); done+=take;
        }
    }
    if(!st) { if(n) memcpy(q->out,stage,n); *q->size=n; }
    free(stage); free(v); return st;
}
static infs_status indexed_read_file(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,uint64_t ordinal,const char *name,
    uint64_t offset,uint8_t *out,size_t capacity,size_t *size,int whole)
{
    if(!l || !name_valid(name) || !size || (!out && capacity))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct indexed_read q={l,io,ordinal,offset,name,out,capacity,size,whole};
    return infs20_paged_read_view(l,io,indexed_read_view,&q);
}
static infs_status volume_recovery_visit(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,infs20_recovery_mapping_fn mapping_fn,
    infs20_recovery_unit_fn unit_fn,void *context)
{
    if(!mapping_fn || !unit_fn) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    unsigned used[INFS20_MODEL_NODES]={0};
    /* Enumerate each pinned namespace, preserving pin role and stable data
     * identity. Only then visit unique data units: clones and snapshots do
     * not require rereading the same underlying physical allocation. */
    for(size_t p=0;p<v.graph.edge_count;++p) {
        const struct infs20_model_edge *e=&v.graph.edges[p];
        if(!zero(e->source,16)) continue;
        for(size_t i=0;i<v.files_count;++i) {
            const struct mapping *m=&v.files[i];
            if(!equal(e->target,m->root)) continue;
            int ci=content(&v,m->data);
            if(ci<0) return INFS_STATUS_CORRUPT;
            struct infs20_recovery_mapping info={0};
            memcpy(info.root_id,m->root,16); memcpy(info.allocation_id,m->data,16);
            info.pin=e->role==INFS20_MODEL_ACTIVE?0:e->ordinal;
            info.role=e->role; info.name=m->name; info.size=v.contents[ci].size;
            st=mapping_fn(context,&info); if(st) return st;
            used[ci]=1;
        }
    }
    for(size_t ci=0;ci<v.contents_count;++ci) {
        if(!used[ci]) continue;
        const struct content *c=&v.contents[ci];
        int d=node(&v,c->id);
        if(d<0) return INFS_STATUS_CORRUPT;
        for(size_t i=0;i<unit_count(c->size);++i) {
            uint8_t page[INFS20_PAGE_BYTES];
            struct infs20_recovery_unit info={0};
            memcpy(info.allocation_id,c->id,16);
            info.physical_block=v.graph.nodes[d].start+i;
            info.logical_offset=i*INFS20_PAGE_BYTES;
            info.logical_size=unit_size(c->size,i);
            info.status=read_unit(&v,io,c->id,i,page);
            info.data=info.status?NULL:page;
            st=unit_fn(context,&info); if(st) return st;
        }
    }
    return INFS_STATUS_OK;
}
static infs_status volume_snapshot(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    if(pin(&v,ordinal)>=0) return INFS_STATUS_ALREADY_EXISTS;
    if(v.graph.edge_count==INFS20_MODEL_EDGES) return INFS_STATUS_NO_SPACE;
    struct infs20_model_edge *e=&v.graph.edges[v.graph.edge_count++];
    memset(e,0,sizeof(*e)); memcpy(e->target,v.graph.edges[pin(&v,0)].target,16);
    e->role=INFS20_MODEL_SNAPSHOT; e->ordinal=ordinal;
    ++v.graph.nodes[node(&v,e->target)].refs;
    return commit(l,paged,io,tx,&v,NULL,0);
}
static infs_status volume_format(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    uint8_t root[16]; id_make(tx,'R',root);
    st=infs20_model_retire_activate(l,&v.graph,root,ordinal);
    return st?st:commit(l,paged,io,tx,&v,NULL,0);
}
static infs_status volume_unformat(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    int p=pin(&v,ordinal); if(p<0 || v.graph.edges[p].role!=INFS20_MODEL_RETIRED)
        return INFS_STATUS_NOT_FOUND;
    /* Verify all payloads that will become visible before changing authority. */
    for(size_t i=0;i<v.files_count;++i)
        if(equal(v.files[i].root,v.graph.edges[p].target)) {
            st=read_content(&v,io,v.files[i].data,NULL); if(st) return st;
        }
    st=infs20_model_reactivate(l,&v.graph,ordinal);
    return st?st:commit(l,paged,io,tx,&v,NULL,0);
}
static infs_status volume_relocate(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,    const struct infs20_store_io *io,const uint8_t tx[16],const char *name)
{
    if(!tx_valid(tx) || !name_valid(name)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    int i=file(&v,v.graph.edges[pin(&v,0)].target,name);
    if(i<0) return INFS_STATUS_NOT_FOUND;
    int ci=content(&v,v.files[i].data);
    if(ci<0) return INFS_STATUS_CORRUPT;
    struct pending *p=calloc(1,sizeof(*p));
    if(!p) return INFS_STATUS_NO_MEMORY;
    p->io=io; p->blocks=unit_count(v.contents[ci].size);
    st=read_content(&v,io,v.files[i].data,p->data);
    if(!st) st=reserve(l,paged,&v,p->blocks,&p->block);
    if(!st) {
        struct infs20_model_node *n=&v.graph.nodes[node(&v,v.files[i].data)];
        /* Old location remains reserved for the preceding bank. */
        st=quarantine_range(&v,n->start,n->blocks);
        if(!st) {
            n->start=p->block;
            st=commit(l,paged,io,tx,&v,p,0);
        }
    }
    free(p);
    return st;
}

/* Explicit GC uses the existing graph transitions; the range ledger
 * retains ownership after removal of the logical node. */
static infs_status collect(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,
    const uint8_t tx[16])
{
    if(!tx_valid(tx)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    if(!v.fully_protected) return INFS_STATUS_BUSY;
    prune_quarantine(&v);
    for(size_t i=0;i<v.graph.node_count;) {
        struct infs20_model_node *n=&v.graph.nodes[i];
        if(n->refs) { ++i; continue; }
        uint8_t id[16]; memcpy(id,n->id,16);
        while(n->state!=INFS20_MODEL_RELEASABLE) {
            st=infs20_model_reclaim_step(l,&v.graph,id);
            if(st) return st;
        }
        if(n->kind==INFS20_MODEL_DATA) {
            int ci=content(&v,id);
            if(ci<0) return INFS_STATUS_CORRUPT;
            st=quarantine_range(&v,n->start,n->blocks);
            if(st) return st;
            memmove(v.contents+ci,v.contents+ci+1,
                    (v.contents_count-(size_t)ci-1)*sizeof(v.contents[0]));
            --v.contents_count;
        }
        /* Logical removal is safe: physical ownership transfers to Q
         * atomically, not directly to the free-space index. */
        st=infs20_model_release(l,&v.graph,id,1);
        if(st) return st;
    }
    return commit(l,paged,io,tx,&v,NULL,0);
}
static infs_status unlink_name(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,
    const uint8_t tx[16],const char *name)
{
    if(!tx_valid(tx) || !name_valid(name)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    if(file(&v,v.graph.edges[pin(&v,0)].target,name)<0) return INFS_STATUS_NOT_FOUND;
    uint8_t root[16]; uint64_t ordinal;
    st=fork_root(l,&v,tx,name,NULL,root,&ordinal);
    return st?st:commit(l,paged,io,tx,&v,NULL,0);
}
/*
 * Rename changes only the namespace mapping. Fork the root once, excluding
 * both names when replacement is requested; the old roots keep their
 * original mappings and the shared allocation identity stays unchanged.
 * Unlike clone+unlink, a failure cannot publish an intermediate duplicate
 * or missing source. No file payload write is needed.
 */
static infs_status rename_name(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,
    const uint8_t tx[16],const char *source,const char *target,int replace)
{
    if(!tx_valid(tx) || !name_valid(source) || !name_valid(target) ||
       (replace!=0 && replace!=1)) return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    const uint8_t *active=v.graph.edges[pin(&v,0)].target;
    int src=file(&v,active,source);
    if(src<0) return INFS_STATUS_NOT_FOUND;
    if(!strcmp(source,target)) return INFS_STATUS_OK;
    int dst=file(&v,active,target);
    if(dst>=0 && !replace) return INFS_STATUS_ALREADY_EXISTS;
    uint8_t data[16],root[16]; uint64_t ordinal;
    memcpy(data,v.files[src].data,16);
    /* Omit both old names while constructing the new root. This avoids
     * manufacturing and then dropping a temporary destination edge;
     * the existing root-drain mechanism is the sole edge-removal path. */
    st=fork_root(l,&v,tx,source,dst>=0?target:NULL,root,&ordinal);
    if(st) return st;
    st=add_file(&v,root,target,data,ordinal);
    return st?st:commit(l,paged,io,tx,&v,NULL,0);
}
static infs_status release_pin(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *paged,const struct infs20_store_io *io,
    const uint8_t tx[16],uint64_t ordinal)
{
    if(!tx_valid(tx) || !ordinal || ordinal==UINT64_MAX)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct volume v; infs_status st=load(l,paged,io,&v); if(st) return st;
    int p=pin(&v,ordinal);
    if(p<0) return INFS_STATUS_NOT_FOUND;
    if(v.graph.edges[p].role!=INFS20_MODEL_SNAPSHOT &&
       v.graph.edges[p].role!=INFS20_MODEL_RETIRED) return INFS_STATUS_BUSY;
    uint8_t root[16]; memcpy(root,v.graph.edges[p].target,16);
    st=infs20_model_drop_pin(l,&v.graph,ordinal); if(st) return st;
    int n=node(&v,root);
    if(n<0) return INFS_STATUS_CORRUPT;
    if(!v.graph.nodes[n].refs) {
        while(v.graph.nodes[n].state!=INFS20_MODEL_RELEASABLE) {
            st=infs20_model_reclaim_step(l,&v.graph,root); if(st) return st;
        }
        size_t kept=0;
        for(size_t i=0;i<v.files_count;++i)
            if(!equal(v.files[i].root,root)) v.files[kept++]=v.files[i];
        v.files_count=kept;
    }
    return commit(l,paged,io,tx,&v,NULL,0);
}
infs_status infs20_paged_volume_collect(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16])
{
    return l?collect(&l->bootstrap,l,io,tx):INFS_STATUS_INVALID_ARGUMENT;
}
infs_status infs20_paged_volume_unlink(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name)
{
    return l?unlink_name(&l->bootstrap,l,io,tx,name):INFS_STATUS_INVALID_ARGUMENT;
}
infs_status infs20_paged_volume_rename(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],
    const char *source,const char *target,int replace)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return rename_name(&l->bootstrap,l,io,tx,source,target,replace);
}
infs_status infs20_paged_volume_release_pin(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    return l?release_pin(&l->bootstrap,l,io,tx,ordinal):INFS_STATUS_INVALID_ARGUMENT;
}

/* Both test profiles share the same semantic engine. Keep the bounded
 * baseline as a regression oracle, not a separate production filesystem. */
infs_status infs20_volume_create(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16])
{
    return volume_create(l,NULL,io,tx,0);
}
infs_status infs20_paged_volume_create(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16])
{
    return infs20_paged_volume_create_at(l,io,tx,0);
}
infs_status infs20_paged_volume_create_at(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t preferred_start)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_create(&l->bootstrap,l,io,tx,preferred_start);
}
infs_status infs20_volume_write(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name,const uint8_t *data,size_t size)
{
    return volume_write(l,NULL,io,tx,name,data,size);
}
infs_status infs20_paged_volume_write(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name,const uint8_t *data,size_t size)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_write(&l->bootstrap,l,io,tx,name,data,size);
}
infs_status infs20_volume_clone(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *source,const char *target)
{
    return volume_clone(l,NULL,io,tx,source,target);
}
infs_status infs20_paged_volume_clone(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *source,const char *target)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_clone(&l->bootstrap,l,io,tx,source,target);
}
infs_status infs20_volume_read(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,uint64_t ordinal,const char *name,uint8_t *out,size_t capacity,size_t *size)
{
    return volume_read(l,NULL,io,ordinal,name,out,capacity,size);
}
infs_status infs20_paged_volume_read(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,uint64_t ordinal,const char *name,uint8_t *out,size_t capacity,size_t *size)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return indexed_read_file(l,io,ordinal,name,0,out,capacity,size,1);
}
infs_status infs20_volume_read_range(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,uint64_t ordinal,const char *name,uint64_t offset,uint8_t *out,size_t capacity,size_t *size)
{
    return volume_read_range(l,NULL,io,ordinal,name,offset,out,capacity,size);
}
infs_status infs20_paged_volume_read_range(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,uint64_t ordinal,const char *name,uint64_t offset,uint8_t *out,size_t capacity,size_t *size)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return indexed_read_file(l,io,ordinal,name,offset,out,capacity,size,0);
}
infs_status infs20_volume_recovery_visit(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,infs20_recovery_mapping_fn mapping_fn,infs20_recovery_unit_fn unit_fn,void *context)
{
    return volume_recovery_visit(l,NULL,io,mapping_fn,unit_fn,context);
}
infs_status infs20_paged_volume_recovery_visit(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,infs20_recovery_mapping_fn mapping_fn,infs20_recovery_unit_fn unit_fn,void *context)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_recovery_visit(&l->bootstrap,l,io,mapping_fn,unit_fn,context);
}
infs_status infs20_volume_snapshot(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    return volume_snapshot(l,NULL,io,tx,ordinal);
}
infs_status infs20_paged_volume_snapshot(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_snapshot(&l->bootstrap,l,io,tx,ordinal);
}
infs_status infs20_volume_format(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    return volume_format(l,NULL,io,tx,ordinal);
}
infs_status infs20_paged_volume_format(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_format(&l->bootstrap,l,io,tx,ordinal);
}
infs_status infs20_volume_unformat(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    return volume_unformat(l,NULL,io,tx,ordinal);
}
infs_status infs20_paged_volume_unformat(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],uint64_t ordinal)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_unformat(&l->bootstrap,l,io,tx,ordinal);
}
infs_status infs20_volume_relocate(const struct infs20_store_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name)
{
    return volume_relocate(l,NULL,io,tx,name);
}
infs_status infs20_paged_volume_relocate(const struct infs20_paged_layout *l,
    const struct infs20_store_io *io,const uint8_t tx[16],const char *name)
{
    if(!l) return INFS_STATUS_INVALID_ARGUMENT;
    return volume_relocate(&l->bootstrap,l,io,tx,name);
}
