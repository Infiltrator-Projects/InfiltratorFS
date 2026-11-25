// SPDX-License-Identifier: GPL-3.0-or-later
#include "space.h"
#include <string.h>

static int nz(const uint8_t *p,size_t n)
{
    uint8_t v=0; while(n--) v|=*p++; return v!=0;
}
static int range_valid(struct infs20_model_range r,uint64_t capacity)
{
    return r.length && r.start<capacity && r.length<=capacity-r.start;
}
/* Call only for independently validated ranges; subtraction avoids overflow. */
static int overlap(struct infs20_model_range a,struct infs20_model_range b)
{
    return a.start<=b.start ? b.start-a.start<a.length : a.start-b.start<b.length;
}
static size_t find_alloc(const struct infs20_space *s,const uint8_t id[16])
{
    for(size_t i=0;i<s->allocations;++i)
        if(!memcmp(s->alloc[i].id,id,16)) return i;
    return s->allocations;
}
static size_t find_edge(const struct infs20_space *s,const uint8_t source[16],uint32_t ord)
{
    for(size_t i=0;i<s->edges;++i)
        if(s->edge[i].ordinal==ord && !memcmp(s->edge[i].source,source,16)) return i;
    return s->edges;
}
static int free_range(const struct infs20_space *s,struct infs20_model_range r)
{
    if(!range_valid(r,s->capacity) || r.start<s->reserved_prefix) return 0;
    for(unsigned i=0;i<3;++i) if(overlap(r,s->arena[i].physical)) return 0;
    for(size_t i=0;i<s->allocations;++i) if(overlap(r,s->alloc[i].physical)) return 0;
    for(size_t i=0;i<s->retired;++i) if(overlap(r,s->old[i].physical)) return 0;
    return 1;
}

infs_status infs20_space_validate(const struct infs20_space *s)
{
    if(!s || !s->capacity || s->capacity>INFS20_MAX_MEMBER_BLOCKS ||
       !s->sequence || !s->oldest_pin || s->oldest_pin>s->sequence ||
       s->reserved_prefix>s->capacity ||
       s->allocations>INFS20_MODEL_ALLOCS || s->edges>INFS20_MODEL_EDGES ||
       s->retired>INFS20_MODEL_RETIRED) return INFS_STATUS_CORRUPT;
    for(unsigned i=0;i<3;++i) {
        const struct infs20_model_arena *a=&s->arena[i];
        if(!range_valid(a->physical,s->capacity) ||
           a->physical.start<s->reserved_prefix || a->frontier>a->physical.length)
            return INFS_STATUS_CORRUPT;
        for(unsigned j=0;j<i;++j) if(overlap(a->physical,s->arena[j].physical))
            return INFS_STATUS_CORRUPT;
    }
    uint32_t counts[INFS20_MODEL_ALLOCS]={0};
    for(size_t i=0;i<s->allocations;++i) {
        const struct infs20_model_alloc *a=&s->alloc[i];
        if(!nz(a->id,16) || !nz(a->digest,32) || !a->decoded_bytes ||
           !range_valid(a->physical,s->capacity) ||
           a->physical.start<s->reserved_prefix ||
           (a->state!=INFS20_ALLOC_LIVE && a->state!=INFS20_ALLOC_PENDING))
            return INFS_STATUS_CORRUPT;
        /* This initial data model permits uncompressed content only. */
        if(a->decoded_bytes>a->physical.length*UINT64_C(4096))
            return INFS_STATUS_CORRUPT;
        if(a->state==INFS20_ALLOC_LIVE && (!a->references || a->retire_sequence))
            return INFS_STATUS_CORRUPT;
        if(a->state==INFS20_ALLOC_PENDING &&
           (a->references || !a->retire_sequence || a->retire_sequence>s->sequence))
            return INFS_STATUS_CORRUPT;
        for(unsigned j=0;j<3;++j)
            if(overlap(a->physical,s->arena[j].physical)) return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<i;++j)
            if(!memcmp(a->id,s->alloc[j].id,16) ||
               overlap(a->physical,s->alloc[j].physical)) return INFS_STATUS_CORRUPT;
    }
    for(size_t i=0;i<s->edges;++i) {
        const struct infs20_model_edge *e=&s->edge[i];
        size_t a=find_alloc(s,e->target);
        if(!nz(e->source,16) || a==s->allocations ||
           s->alloc[a].state!=INFS20_ALLOC_LIVE) return INFS_STATUS_CORRUPT;
        ++counts[a];
        for(size_t j=0;j<i;++j)
            if(e->ordinal==s->edge[j].ordinal &&
               !memcmp(e->source,s->edge[j].source,16)) return INFS_STATUS_CORRUPT;
    }
    for(size_t i=0;i<s->allocations;++i)
        if(counts[i]!=s->alloc[i].references) return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<s->retired;++i) {
        const struct infs20_model_retired *r=&s->old[i];
        if(!range_valid(r->physical,s->capacity) ||
           r->physical.start<s->reserved_prefix || !r->sequence ||
           r->sequence>s->sequence) return INFS_STATUS_CORRUPT;
        for(unsigned j=0;j<3;++j)
            if(overlap(r->physical,s->arena[j].physical)) return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<s->allocations;++j)
            if(overlap(r->physical,s->alloc[j].physical)) return INFS_STATUS_CORRUPT;
        for(size_t j=0;j<i;++j)
            if(overlap(r->physical,s->old[j].physical)) return INFS_STATUS_CORRUPT;
    }
    return INFS_STATUS_OK;
}
infs_status infs20_space_init(struct infs20_space *s,uint64_t capacity,
                              uint64_t reserved_prefix,
                              const struct infs20_model_range arenas[3])
{
    if(!s || !arenas) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_space t={0}; t.capacity=capacity; t.reserved_prefix=reserved_prefix;
    t.sequence=t.oldest_pin=1;
    for(unsigned i=0;i<3;++i) t.arena[i].physical=arenas[i];
    if(infs20_space_validate(&t)) return INFS_STATUS_INVALID_ARGUMENT;
    *s=t; return INFS_STATUS_OK;
}
infs_status infs20_space_begin(const struct infs20_space *current,
                               struct infs20_space *next)
{
    if(!current || !next || current==next) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(current)) return INFS_STATUS_CORRUPT;
    if(current->sequence==UINT64_MAX) return INFS_STATUS_OVERFLOW;
    *next=*current; ++next->sequence; return INFS_STATUS_OK;
}
infs_status infs20_space_add(struct infs20_space *s,const uint8_t id[16],
                             const uint8_t digest[32],uint64_t decoded_bytes,
                             struct infs20_model_range physical,
                             const uint8_t source[16],uint32_t ordinal)
{
    if(!s || !id || !digest || !source || !nz(id,16) || !nz(digest,32) ||
       !nz(source,16) || !decoded_bytes) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    if(find_alloc(s,id)!=s->allocations || find_edge(s,source,ordinal)!=s->edges)
        return INFS_STATUS_ALREADY_EXISTS;
    if(s->allocations==INFS20_MODEL_ALLOCS || s->edges==INFS20_MODEL_EDGES ||
       !free_range(s,physical)) return INFS_STATUS_NO_SPACE;
    if(decoded_bytes>physical.length*UINT64_C(4096)) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_model_alloc a={0};
    memcpy(a.id,id,16); memcpy(a.digest,digest,32); a.decoded_bytes=decoded_bytes;
    a.physical=physical; a.references=1; a.state=INFS20_ALLOC_LIVE;
    struct infs20_model_edge e={0};
    memcpy(e.source,source,16); memcpy(e.target,id,16); e.ordinal=ordinal;
    s->alloc[s->allocations++]=a; s->edge[s->edges++]=e;
    return INFS_STATUS_OK;
}
infs_status infs20_space_link(struct infs20_space *s,const uint8_t target[16],
                              const uint8_t source[16],uint32_t ordinal)
{
    if(!s || !target || !source || !nz(source,16)) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    size_t a=find_alloc(s,target);
    if(a==s->allocations || s->alloc[a].state!=INFS20_ALLOC_LIVE)
        return INFS_STATUS_NOT_FOUND;
    if(find_edge(s,source,ordinal)!=s->edges) return INFS_STATUS_ALREADY_EXISTS;
    if(s->edges==INFS20_MODEL_EDGES) return INFS_STATUS_NO_SPACE;
    struct infs20_model_edge e={0};
    memcpy(e.source,source,16); memcpy(e.target,target,16); e.ordinal=ordinal;
    s->edge[s->edges++]=e; ++s->alloc[a].references;
    return INFS_STATUS_OK;
}
infs_status infs20_space_unlink(struct infs20_space *s,const uint8_t source[16],
                                uint32_t ordinal)
{
    if(!s || !source) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    size_t e=find_edge(s,source,ordinal);
    if(e==s->edges) return INFS_STATUS_NOT_FOUND;
    size_t a=find_alloc(s,s->edge[e].target);
    if(--s->alloc[a].references==0) {
        s->alloc[a].state=INFS20_ALLOC_PENDING;
        s->alloc[a].retire_sequence=s->sequence;
    }
    --s->edges; s->edge[e]=s->edge[s->edges];
    memset(&s->edge[s->edges],0,sizeof(s->edge[0]));
    return INFS_STATUS_OK;
}
infs_status infs20_space_relocate(struct infs20_space *s,const uint8_t target[16],
                                  struct infs20_model_range physical)
{
    if(!s || !target) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    size_t a=find_alloc(s,target);
    if(a==s->allocations || s->alloc[a].state!=INFS20_ALLOC_LIVE)
        return INFS_STATUS_NOT_FOUND;
    if(s->retired==INFS20_MODEL_RETIRED || !free_range(s,physical))
        return INFS_STATUS_NO_SPACE;
    if(physical.length!=s->alloc[a].physical.length)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_model_retired old={s->alloc[a].physical,s->sequence};
    s->old[s->retired++]=old;
    s->alloc[a].physical=physical;
    return INFS_STATUS_OK;
}
infs_status infs20_space_advance_pin(struct infs20_space *s,uint64_t oldest)
{
    if(!s) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    if(oldest<s->oldest_pin || oldest>s->sequence) return INFS_STATUS_INVALID_ARGUMENT;
    s->oldest_pin=oldest; return INFS_STATUS_OK;
}
infs_status infs20_space_collect(struct infs20_space *s,size_t limit,size_t *released)
{
    if(!s || !released) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    size_t n=0;
    for(size_t i=0;i<s->allocations && n<limit;) {
        struct infs20_model_alloc *a=&s->alloc[i];
        if(a->state==INFS20_ALLOC_PENDING && a->retire_sequence<=s->oldest_pin) {
            --s->allocations; *a=s->alloc[s->allocations];
            memset(&s->alloc[s->allocations],0,sizeof(*a)); ++n;
        } else ++i;
    }
    for(size_t i=0;i<s->retired && n<limit;) {
        if(s->old[i].sequence<=s->oldest_pin) {
            --s->retired; s->old[i]=s->old[s->retired];
            memset(&s->old[s->retired],0,sizeof(s->old[0])); ++n;
        } else ++i;
    }
    *released=n; return INFS_STATUS_OK;
}
infs_status infs20_space_arena_reserve(struct infs20_space *s,uint64_t blocks,
                                       uint64_t starts[3])
{
    if(!s || !starts || !blocks) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(s)) return INFS_STATUS_CORRUPT;
    uint64_t proposed[3];
    for(unsigned i=0;i<3;++i) {
        struct infs20_model_arena *a=&s->arena[i];
        if(blocks>a->physical.length-a->frontier) return INFS_STATUS_NO_SPACE;
        proposed[i]=a->physical.start+a->frontier;
    }
    for(unsigned i=0;i<3;++i) s->arena[i].frontier+=blocks;
    memcpy(starts,proposed,sizeof(proposed)); return INFS_STATUS_OK;
}
