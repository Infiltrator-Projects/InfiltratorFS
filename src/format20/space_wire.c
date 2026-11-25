// SPDX-License-Identifier: GPL-3.0-or-later
#include "space_wire.h"
#include "infilfs/endian.h"
#include <stdlib.h>
#include <string.h>

enum { ALLOCS, LOCATIONS, OWNERS, EDGES, ARENAS, RETIRED };
#define MODEL_ROWS 65u
#define MODEL_VALUE_BYTES 80u
struct rows {
    struct infs20_tree_entry entry[MODEL_ROWS];
    uint8_t value[MODEL_ROWS][MODEL_VALUE_BYTES];
    size_t count;
};
_Static_assert(INFS20_MODEL_EDGES<=MODEL_ROWS,"edge capacity");
_Static_assert(INFS20_MODEL_ALLOCS+INFS20_MODEL_RETIRED+4<=MODEL_ROWS,
               "owner capacity");

/* The ordinal key is big-endian only as a byte-order-preserving index,
 * not a persisted numeric field. All record numeric values are LE. */
static void ordinal_key(uint8_t key[16],size_t ordinal)
{
    memset(key,0,16);
    key[14]=(uint8_t)(ordinal>>8); key[15]=(uint8_t)ordinal;
}
static uint8_t *row(struct rows *r,uint16_t length)
{
    size_t i=r->count++;
    ordinal_key(r->entry[i].key,i);
    r->entry[i].value=r->value[i]; r->entry[i].value_size=length;
    memset(r->value[i],0,MODEL_VALUE_BYTES);
    return r->value[i];
}
static void range_put(uint8_t *p,struct infs20_model_range r)
{
    infs_store_le64(p,r.start); infs_store_le64(p+8,r.length);
}
static struct infs20_model_range range_get(const uint8_t *p)
{
    struct infs20_model_range r={infs_load_le64(p),infs_load_le64(p+8)};
    return r;
}
static int alloc_cmp(const void *a,const void *b)
{
    return memcmp(((const struct infs20_model_alloc *)a)->id,
                  ((const struct infs20_model_alloc *)b)->id,16);
}
static int edge_cmp(const void *a,const void *b)
{
    const struct infs20_model_edge *x=a,*y=b;
    int c=memcmp(x->source,y->source,16);
    return c?c:(x->ordinal>y->ordinal)-(x->ordinal<y->ordinal);
}
static int retired_cmp(const void *a,const void *b)
{
    uint64_t x=((const struct infs20_model_retired *)a)->physical.start;
    uint64_t y=((const struct infs20_model_retired *)b)->physical.start;
    return (x>y)-(x<y);
}
static void owner_row(struct rows *r,uint8_t kind,struct infs20_model_range range,
                      const uint8_t *id,uint64_t seq)
{
    uint8_t *v=row(r,41);
    v[0]=kind; range_put(v+1,range);
    if(id) memcpy(v+17,id,16);
    infs_store_le64(v+33,seq);
}
infs_status infs20_space_pack(const struct infs20_space *state,
                              struct infs20_space_wire *out)
{
    if(!state || !out) return INFS_STATUS_INVALID_ARGUMENT;
    if(infs20_space_validate(state)) return INFS_STATUS_CORRUPT;
    struct infs20_space s=*state;
    /* Canonical physical representation is independent of insertion order
     * or swap-with-last reclamation. */
    qsort(s.alloc,s.allocations,sizeof(s.alloc[0]),alloc_cmp);
    qsort(s.edge,s.edges,sizeof(s.edge[0]),edge_cmp);
    qsort(s.old,s.retired,sizeof(s.old[0]),retired_cmp);
    struct infs20_space_wire wire={0};
    for(unsigned role=0;role<INFS20_SPACE_ROOTS;++role) {
        struct rows r={0};
        if(role==ALLOCS) {
            for(size_t i=0;i<s.allocations;++i) {
                const struct infs20_model_alloc *a=&s.alloc[i];
                uint8_t *v=row(&r,69);
                memcpy(v,a->id,16); memcpy(v+16,a->digest,32);
                infs_store_le64(v+48,a->decoded_bytes);
                infs_store_le32(v+56,a->references); v[60]=a->state;
                infs_store_le64(v+61,a->retire_sequence);
            }
        } else if(role==LOCATIONS) {
            for(size_t i=0;i<s.allocations;++i) {
                uint8_t *v=row(&r,32);
                memcpy(v,s.alloc[i].id,16); range_put(v+16,s.alloc[i].physical);
            }
        } else if(role==OWNERS) {
            if(s.reserved_prefix)
                owner_row(&r,1,(struct infs20_model_range){0,s.reserved_prefix},NULL,0);
            for(unsigned i=0;i<3;++i)
                owner_row(&r,2,s.arena[i].physical,NULL,0);
            for(size_t i=0;i<s.allocations;++i)
                owner_row(&r,3,s.alloc[i].physical,s.alloc[i].id,s.alloc[i].retire_sequence);
            for(size_t i=0;i<s.retired;++i)
                owner_row(&r,4,s.old[i].physical,NULL,s.old[i].sequence);
        } else if(role==EDGES) {
            for(size_t i=0;i<s.edges;++i) {
                uint8_t *v=row(&r,36); memcpy(v,s.edge[i].source,16);
                memcpy(v+16,s.edge[i].target,16); infs_store_le32(v+32,s.edge[i].ordinal);
            }
        } else if(role==ARENAS) {
            uint8_t *v=row(&r,32);
            infs_store_le64(v,s.capacity); infs_store_le64(v+8,s.sequence);
            infs_store_le64(v+16,s.oldest_pin); infs_store_le64(v+24,s.reserved_prefix);
            for(unsigned i=0;i<3;++i) {
                v=row(&r,24); range_put(v,s.arena[i].physical);
                infs_store_le64(v+16,s.arena[i].frontier);
            }
        } else {
            for(size_t i=0;i<s.retired;++i) {
                uint8_t *v=row(&r,24); range_put(v,s.old[i].physical);
                infs_store_le64(v+16,s.old[i].sequence);
            }
        }
        infs_status st=infs20_tree_encode(wire.payload[role],INFS20_PAYLOAD_BYTES,
                                         0,r.entry,r.count,&wire.size[role]);
        if(st) return st;
    }
    *out=wire; return INFS_STATUS_OK;
}

/* Framing is checked independently of the enclosing page verifier, because
 * callers may use this decoder on an offline reconstructed payload. */
struct cursor { const uint8_t *p; size_t len,pos,count,index; };
static int cursor_init(struct cursor *c,const struct infs20_space_wire *w,unsigned role)
{
    c->p=w->payload[role]; c->len=w->size[role]; c->pos=8; c->index=0;
    if(c->len<8 || c->len>INFS20_PAYLOAD_BYTES || infs_load_le16(c->p)!=0 ||
       infs_load_le32(c->p+4)!=c->len) return 0;
    c->count=infs_load_le16(c->p+2);
    return c->count<=MODEL_ROWS;
}
static const uint8_t *take(struct cursor *c,size_t n)
{
    uint8_t key[16]; ordinal_key(key,c->index);
    if(c->index>=c->count || c->pos>c->len || c->len-c->pos<18 ||
       n>c->len-c->pos-18 || memcmp(c->p+c->pos,key,16) ||
       infs_load_le16(c->p+c->pos+16)!=n) return NULL;
    const uint8_t *v=c->p+c->pos+18;
    c->pos+=18+n; ++c->index; return v;
}
static int done(const struct cursor *c)
{
    return c->index==c->count && c->pos==c->len;
}
infs_status infs20_space_unpack(const struct infs20_space_wire *w,
                                uint64_t capacity,uint64_t sequence,
                                struct infs20_space *out)
{
    if(!w || !out || !capacity || !sequence) return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_space s={0};
    struct cursor c;
    const uint8_t *v;
    if(!cursor_init(&c,w,ARENAS) || c.count!=4 || !(v=take(&c,32)))
        return INFS_STATUS_CORRUPT;
    s.capacity=infs_load_le64(v); s.sequence=infs_load_le64(v+8);
    s.oldest_pin=infs_load_le64(v+16); s.reserved_prefix=infs_load_le64(v+24);
    if(s.capacity!=capacity || s.sequence!=sequence) return INFS_STATUS_CORRUPT;
    for(unsigned i=0;i<3;++i) {
        if(!(v=take(&c,24))) return INFS_STATUS_CORRUPT;
        s.arena[i].physical=range_get(v); s.arena[i].frontier=infs_load_le64(v+16);
    }
    if(!done(&c) || !cursor_init(&c,w,ALLOCS) || c.count>INFS20_MODEL_ALLOCS)
        return INFS_STATUS_CORRUPT;
    s.allocations=c.count;
    for(size_t i=0;i<s.allocations;++i) {
        if(!(v=take(&c,69))) return INFS_STATUS_CORRUPT;
        struct infs20_model_alloc *a=&s.alloc[i];
        memcpy(a->id,v,16); memcpy(a->digest,v+16,32);
        a->decoded_bytes=infs_load_le64(v+48); a->references=infs_load_le32(v+56);
        a->state=v[60]; a->retire_sequence=infs_load_le64(v+61);
    }
    if(!done(&c) || !cursor_init(&c,w,LOCATIONS) || c.count!=s.allocations)
        return INFS_STATUS_CORRUPT;
    for(size_t i=0;i<s.allocations;++i) {
        if(!(v=take(&c,32)) || memcmp(v,s.alloc[i].id,16))
            return INFS_STATUS_CORRUPT;
        s.alloc[i].physical=range_get(v+16);
    }
    if(!done(&c) || !cursor_init(&c,w,EDGES) || c.count>INFS20_MODEL_EDGES)
        return INFS_STATUS_CORRUPT;
    s.edges=c.count;
    for(size_t i=0;i<s.edges;++i) {
        if(!(v=take(&c,36))) return INFS_STATUS_CORRUPT;
        memcpy(s.edge[i].source,v,16); memcpy(s.edge[i].target,v+16,16);
        s.edge[i].ordinal=infs_load_le32(v+32);
    }
    if(!done(&c) || !cursor_init(&c,w,RETIRED) || c.count>INFS20_MODEL_RETIRED)
        return INFS_STATUS_CORRUPT;
    s.retired=c.count;
    for(size_t i=0;i<s.retired;++i) {
        if(!(v=take(&c,24))) return INFS_STATUS_CORRUPT;
        s.old[i].physical=range_get(v); s.old[i].sequence=infs_load_le64(v+16);
    }
    if(!done(&c) || infs20_space_validate(&s)) return INFS_STATUS_CORRUPT;
    /* Rebuild the redundant owner index from independent records and enforce
     * canonical ordering/framing of every root. A valid page digest alone
     * cannot authorise a conflicting free-space or ownership decision. */
    struct infs20_space_wire rebuilt;
    if(infs20_space_pack(&s,&rebuilt)) return INFS_STATUS_CORRUPT;
    for(unsigned i=0;i<INFS20_SPACE_ROOTS;++i)
        if(w->size[i]!=rebuilt.size[i] || memcmp(w->payload[i],rebuilt.payload[i],rebuilt.size[i]))
            return INFS_STATUS_CORRUPT;
    *out=s; return INFS_STATUS_OK;
}
infs_status infs20_space_read(const struct infs20_io *io,
                              const struct infs20_authority *a,
                              size_t budget,struct infs20_space *out)
{
    if(!io || !a || !out || a->commit.blocks!=io->blocks)
        return INFS_STATUS_INVALID_ARGUMENT;
    struct infs20_space_wire w={0};
    for(unsigned i=0;i<INFS20_SPACE_ROOTS;++i) {
        infs_status st=infs20_read_leaf(io,&a->commit.roots[INFS20_ROOT_ALLOCATIONS+i],
                                        a->sequence,&budget,w.payload[i],
                                        sizeof(w.payload[i]),&w.size[i]);
        if(st) return st;
    }
    return infs20_space_unpack(&w,io->blocks,a->sequence,out);
}
