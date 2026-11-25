// SPDX-License-Identifier: GPL-3.0-or-later
#include "tree.h"
#include "infilfs/endian.h"
#include <stdlib.h>
#include <string.h>

/*
 * A branch entry stores the exact minimum key of its child and an immutable
 * reference (identity, logical digest, birth sequence, direct replica
 * addresses). The complete parent payload binds those addresses. Physical
 * placement of the parent itself is excluded by the common envelope.
 * Relocating a metadata child therefore copies its affected ancestor path;
 * data relocation remains a separate allocation-location operation.
 *
 * Wire payload: "T20B", level LE16, count LE16, then ordered entries:
 * key length LE16, value/reference length LE16, key, value/reference.
 * Branch references are fixed 80 bytes. Level decreases at every edge,
 * so malformed cycles cannot create unbounded recursion. Leaf records
 * are canonical typed keys/values at the caller boundary. The bytewise
 * comparator is explicit: numeric keys need order-preserving encoding.
 */
enum { ENTRY_HEADER=4, MAX_ENTRIES=(INFS20_PAYLOAD_BYTES-INFS20_TREE_NODE_HEADER)/5 };
struct entry {
    const uint8_t *key,*value;
    uint16_t key_size,value_size;
};
struct node {
    uint8_t payload[INFS20_PAYLOAD_BYTES];
    struct entry entries[MAX_ENTRIES+1];
    uint16_t level,count;
};
struct child {
    struct infs20_tree_ref ref;
    uint8_t minimum[INFS20_TREE_KEY_MAX];
    uint16_t size,level;
};
struct result {
    struct child child[2];
    unsigned count;
};
struct bound {
    const uint8_t *key;
    size_t size;
};
static int zero(const uint8_t *p,size_t n)
{
    uint8_t v=0; while(n--) v|=*p++; return v==0;
}
static int keycmp(const uint8_t *a,size_t an,const uint8_t *b,size_t bn)
{
    size_t n=an<bn?an:bn;
    int c=memcmp(a,b,n);
    return c?c:(an>bn)-(an<bn);
}
static int empty(const struct infs20_tree_ref *r)
{
    return zero(r->id,16) && zero(r->digest,32) && !r->sequence &&
        !r->blocks[0] && !r->blocks[1] && !r->blocks[2];
}
static int io_valid(const struct infs20_tree_io *io,int write)
{
    return io && io->read && (!write || (io->write && io->reserve)) &&
        !zero(io->set_id,16) && !zero(io->scope_id,16) &&
        !zero(io->member_id,16) && io->sequence && io->member_blocks>=3 &&
        io->member_blocks<=INFS20_MAX_MEMBER_BLOCKS;
}
static int ref_valid(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *r,uint64_t ceiling)
{
    if(zero(r->id,16) || zero(r->digest,32) || !r->sequence ||
       r->sequence>ceiling) return 0;
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i) {
        if(r->blocks[i]>=io->member_blocks) return 0;
        for(unsigned j=0;j<i;++j) if(r->blocks[i]==r->blocks[j]) return 0;
    }
    return 1;
}
static void ref_encode(uint8_t out[INFS20_TREE_REF_BYTES],
    const struct infs20_tree_ref *r)
{
    memcpy(out,r->id,16); memcpy(out+16,r->digest,32);
    infs_store_le64(out+48,r->sequence);
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i)
        infs_store_le64(out+56+8*i,r->blocks[i]);
}
static void ref_decode(const uint8_t *p,struct infs20_tree_ref *r)
{
    memcpy(r->id,p,16); memcpy(r->digest,p+16,32);
    r->sequence=infs_load_le64(p+48);
    for(unsigned i=0;i<INFS20_LANE_COUNT;++i)
        r->blocks[i]=infs_load_le64(p+56+8*i);
}
static size_t entry_bytes(const struct entry *e)
{
    return ENTRY_HEADER+(size_t)e->key_size+e->value_size;
}
static size_t used(const struct entry *e,size_t count)
{
    size_t n=INFS20_TREE_NODE_HEADER;
    for(size_t i=0;i<count;++i) n+=entry_bytes(e+i);
    return n;
}
static infs_status parse(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *r,struct node *n,size_t size)
{
    if(size<INFS20_TREE_NODE_HEADER || memcmp(n->payload,"T20B",4))
        return INFS_STATUS_CORRUPT;
    n->level=infs_load_le16(n->payload+4);
    n->count=infs_load_le16(n->payload+6);
    if(n->level>INFS20_TREE_MAX_LEVEL || !n->count || n->count>MAX_ENTRIES)
        return INFS_STATUS_CORRUPT;
    size_t at=INFS20_TREE_NODE_HEADER;
    for(unsigned i=0;i<n->count;++i) {
        if(size-at<ENTRY_HEADER) return INFS_STATUS_CORRUPT;
        uint16_t k=infs_load_le16(n->payload+at),
                 v=infs_load_le16(n->payload+at+2);
        at+=ENTRY_HEADER;
        if(!k || k>INFS20_TREE_KEY_MAX || k>size-at || v>size-at-k)
            return INFS_STATUS_CORRUPT;
        struct entry *e=&n->entries[i];
        *e=(struct entry){n->payload+at,n->payload+at+k,k,v};
        if(i && keycmp(n->entries[i-1].key,n->entries[i-1].key_size,e->key,k)>=0)
            return INFS_STATUS_CORRUPT;
        if(n->level) {
            struct infs20_tree_ref child;
            if(v!=INFS20_TREE_REF_BYTES) return INFS_STATUS_CORRUPT;
            ref_decode(e->value,&child);
            if(!ref_valid(io,&child,r->sequence))
                return INFS_STATUS_CORRUPT;
        }
        at+=(size_t)k+v;
    }
    return at==size?INFS_STATUS_OK:INFS_STATUS_CORRUPT;
}
static infs_status read_node(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *r,struct node *n)
{
    if(!ref_valid(io,r,io->sequence)) return INFS_STATUS_CORRUPT;
    uint8_t page[INFS20_PAGE_BYTES];
    infs_status operational=INFS_STATUS_OK;
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
        infs_status st=io->read(io->context,r->blocks[lane],page);
        if(st) { if(st!=INFS_STATUS_CORRUPT) operational=st; continue; }
        struct infs20_decoded_page d;
        st=infs20_page_decode(page,sizeof(page),io->member_id,r->blocks[lane],
                               io->member_blocks,&d);
        if(st) {
            if(st==INFS_STATUS_NOT_SUPPORTED && !memcmp(page,"INF20S0!",8))
                operational=st;
            continue;
        }
        if(d.identity.lane!=lane || d.identity.sequence!=r->sequence ||
           memcmp(d.identity.set_id,io->set_id,16) ||
           memcmp(d.identity.scope_id,io->scope_id,16) ||
           memcmp(d.identity.page_id,r->id,16) || memcmp(d.digest,r->digest,32))
            continue;
        memcpy(n->payload,page+INFS20_OFF_PAYLOAD,d.payload_size);
        /* Matching immutable digest binds the same payload on every lane. */
        return parse(io,r,n,d.payload_size);
    }
    return operational?operational:INFS_STATUS_CORRUPT;
}
static infs_status bounds(const struct node *n,int level,
    struct bound lower,struct bound upper)
{
    if((level>=0 && n->level!=(unsigned)level) ||
       (lower.key && keycmp(n->entries[0].key,n->entries[0].key_size,
                            lower.key,lower.size)!=0) ||
       (upper.key && keycmp(n->entries[n->count-1].key,
                            n->entries[n->count-1].key_size,upper.key,upper.size)>=0))
        return INFS_STATUS_CORRUPT;
    return INFS_STATUS_OK;
}
static unsigned position(const struct node *n,const uint8_t *key,size_t size)
{
    unsigned lo=0,hi=n->count;
    while(lo<hi) {
        unsigned mid=lo+(hi-lo)/2;
        if(keycmp(n->entries[mid].key,n->entries[mid].key_size,key,size)<0)
            lo=mid+1;
        else hi=mid;
    }
    return lo;
}
static unsigned route(const struct node *n,const uint8_t *key,size_t size)
{
    unsigned i=position(n,key,size);
    if(i<n->count && !keycmp(n->entries[i].key,n->entries[i].key_size,key,size))
        return i;
    return i?i-1:0;
}
static struct bound entry_bound(const struct entry *e)
{
    return (struct bound){e->key,e->key_size};
}
static infs_status emit(const struct infs20_tree_io *io,uint16_t level,
    const struct entry *e,size_t count,struct child *out)
{
    if(!count || count>MAX_ENTRIES || used(e,count)>INFS20_PAYLOAD_BYTES ||
       level>INFS20_TREE_MAX_LEVEL) return INFS_STATUS_NO_SPACE;
    uint8_t payload[INFS20_PAYLOAD_BYTES]={0},page[INFS20_PAGE_BYTES];
    memcpy(payload,"T20B",4);
    infs_store_le16(payload+4,level); infs_store_le16(payload+6,count);
    size_t at=INFS20_TREE_NODE_HEADER;
    for(size_t i=0;i<count;++i) {
        infs_store_le16(payload+at,e[i].key_size);
        infs_store_le16(payload+at+2,e[i].value_size);
        at+=ENTRY_HEADER;
        memcpy(payload+at,e[i].key,e[i].key_size); at+=e[i].key_size;
        if(e[i].value_size) memcpy(payload+at,e[i].value,e[i].value_size);
        at+=e[i].value_size;
    }
    struct child c={0}; c.ref.sequence=io->sequence;
    infs_status st=io->reserve(io->context,c.ref.id,c.ref.blocks);
    if(st) return st;
    /* Reservation ownership is proved by the allocator contract. These
     * checks also reject malformed callback output before any write. */
    if(zero(c.ref.id,16)) return INFS_STATUS_CORRUPT;
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
        if(c.ref.blocks[lane]>=io->member_blocks) return INFS_STATUS_CORRUPT;
        for(unsigned j=0;j<lane;++j)
            if(c.ref.blocks[lane]==c.ref.blocks[j]) return INFS_STATUS_CORRUPT;
    }
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
        struct infs20_page_identity id={0};
        memcpy(id.set_id,io->set_id,16); memcpy(id.scope_id,io->scope_id,16);
        memcpy(id.member_id,io->member_id,16); memcpy(id.page_id,c.ref.id,16);
        id.sequence=io->sequence; id.block=c.ref.blocks[lane]; id.lane=(uint16_t)lane;
        st=infs20_page_encode(page,sizeof(page),&id,payload,at);
        if(st) return st;
        if(!lane) memcpy(c.ref.digest,page+INFS20_OFF_DIGEST,32);
        else if(memcmp(c.ref.digest,page+INFS20_OFF_DIGEST,32))
            return INFS_STATUS_CORRUPT;
        st=io->write(io->context,id.block,page);
        if(st) return st;
    }
    c.level=level; c.size=e[0].key_size;
    memcpy(c.minimum,e[0].key,c.size);
    *out=c;
    return INFS_STATUS_OK;
}
static infs_status emit_split(const struct infs20_tree_io *io,uint16_t level,
    const struct entry *e,size_t count,struct result *out)
{
    struct result r={0};
    if(!count) { *out=r; return INFS_STATUS_OK; }
    if(used(e,count)<=INFS20_PAYLOAD_BYTES) {
        infs_status st=emit(io,level,e,count,&r.child[0]);
        if(st) return st;
        r.count=1; *out=r; return INFS_STATUS_OK;
    }
    /* Byte-balanced split: variable-size names and values cannot be
     * balanced by a fixed entry count. Both halves must encode. */
    size_t split=0,best=SIZE_MAX,left=INFS20_TREE_NODE_HEADER;
    size_t total=used(e,count);
    for(size_t i=1;i<count;++i) {
        left+=entry_bytes(e+i-1);
        size_t right=total-left+INFS20_TREE_NODE_HEADER;
        if(left>INFS20_PAYLOAD_BYTES || right>INFS20_PAYLOAD_BYTES) continue;
        size_t diff=left>right?left-right:right-left;
        if(diff<best) { best=diff; split=i; }
    }
    if(!split) return INFS_STATUS_NO_SPACE;
    infs_status st=emit(io,level,e,split,&r.child[0]);
    if(!st) st=emit(io,level,e+split,count-split,&r.child[1]);
    if(st) return st;
    r.count=2; *out=r;
    return INFS_STATUS_OK;
}
static infs_status mutate(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *old,int level,struct bound lower,
    struct bound upper,const uint8_t *key,size_t key_size,
    const uint8_t *value,size_t value_size,int remove,int root,
    struct result *out)
{
    struct node *n=calloc(1,sizeof(*n));
    if(!n) return INFS_STATUS_NO_MEMORY;
    infs_status st=read_node(io,old,n);
    if(!st) st=bounds(n,level,lower,upper);
    if(st) goto done;
    unsigned at=position(n,key,key_size);
    if(!n->level) {
        int found=at<n->count && !keycmp(n->entries[at].key,
                                        n->entries[at].key_size,key,key_size);
        if(remove) {
            if(!found) { st=INFS_STATUS_NOT_FOUND; goto done; }
            memmove(n->entries+at,n->entries+at+1,
                    (n->count-at-1)*sizeof(n->entries[0]));
            --n->count;
        } else {
            if(!found) {
                memmove(n->entries+at+1,n->entries+at,
                        (n->count-at)*sizeof(n->entries[0]));
                ++n->count;
            }
            n->entries[at]=(struct entry){key,value,(uint16_t)key_size,(uint16_t)value_size};
        }
        st=emit_split(io,0,n->entries,n->count,out);
        goto done;
    }
    at=route(n,key,key_size);
    struct infs20_tree_ref child;
    ref_decode(n->entries[at].value,&child);
    struct bound child_upper=at+1<n->count?entry_bound(n->entries+at+1):upper;
    struct result replacement={0};
    st=mutate(io,&child,n->level-1,entry_bound(n->entries+at),child_upper,
              key,key_size,value,value_size,remove,0,&replacement);
    if(st) goto done;
    /* Exactly one child edge is replaced by zero, one or two new edges. */
    memmove(n->entries+at+replacement.count,n->entries+at+1,
            (n->count-at-1)*sizeof(n->entries[0]));
    n->count=(uint16_t)(n->count-1+replacement.count);
    uint8_t refs[2][INFS20_TREE_REF_BYTES];
    for(unsigned i=0;i<replacement.count;++i) {
        ref_encode(refs[i],&replacement.child[i].ref);
        n->entries[at+i]=(struct entry){replacement.child[i].minimum,refs[i],
                                       replacement.child[i].size,INFS20_TREE_REF_BYTES};
    }
    if(root && n->count==1) {
        /* Contraction is safe because the sole child reference already
         * proves membership. No extra one-child root is written. */
        struct result r={0}; r.count=1;
        ref_decode(n->entries[0].value,&r.child[0].ref);
        r.child[0].level=n->level-1;
        r.child[0].size=n->entries[0].key_size;
        memcpy(r.child[0].minimum,n->entries[0].key,r.child[0].size);
        *out=r; st=INFS_STATUS_OK;
    } else st=emit_split(io,n->level,n->entries,n->count,out);
done:
    free(n);
    return st;
}
static infs_status change(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *old,const uint8_t *key,size_t key_size,
    const uint8_t *value,size_t value_size,int remove,struct infs20_tree_ref *out)
{
    if(!io_valid(io,1) || !old || !out || !key || !key_size ||
       key_size>INFS20_TREE_KEY_MAX || (value_size && !value))
        return INFS_STATUS_INVALID_ARGUMENT;
    if(value_size>INFS20_PAYLOAD_BYTES-INFS20_TREE_NODE_HEADER-ENTRY_HEADER-key_size)
        return INFS_STATUS_NO_SPACE;
    struct result r={0}; infs_status st;
    if(empty(old)) {
        if(remove) return INFS_STATUS_NOT_FOUND;
        struct entry e={key,value,(uint16_t)key_size,(uint16_t)value_size};
        st=emit_split(io,0,&e,1,&r);
    } else st=mutate(io,old,-1,(struct bound){0},(struct bound){0},
                     key,key_size,value,value_size,remove,1,&r);
    if(st) return st;
    struct infs20_tree_ref next={0};
    if(r.count==1) next=r.child[0].ref;
    else if(r.count==2) {
        if(r.child[0].level==INFS20_TREE_MAX_LEVEL) return INFS_STATUS_NO_SPACE;
        uint8_t refs[2][INFS20_TREE_REF_BYTES];
        struct entry e[2];
        for(unsigned i=0;i<2;++i) {
            ref_encode(refs[i],&r.child[i].ref);
            e[i]=(struct entry){r.child[i].minimum,refs[i],
                                r.child[i].size,INFS20_TREE_REF_BYTES};
        }
        struct child root;
        st=emit(io,(uint16_t)(r.child[0].level+1),e,2,&root);
        if(st) return st;
        next=root.ref;
    }
    *out=next;
    return INFS_STATUS_OK;
}
infs_status infs20_tree_put(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *old,const uint8_t *key,size_t key_size,
    const uint8_t *value,size_t value_size,struct infs20_tree_ref *out)
{
    return change(io,old,key,key_size,value,value_size,0,out);
}
infs_status infs20_tree_remove(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *old,const uint8_t *key,size_t key_size,
    struct infs20_tree_ref *out)
{
    return change(io,old,key,key_size,NULL,0,1,out);
}
infs_status infs20_tree_get(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *root,const uint8_t *key,size_t key_size,
    uint8_t *out,size_t capacity,size_t *size)
{
    if(!io_valid(io,0) || !root || !key || !key_size ||
       key_size>INFS20_TREE_KEY_MAX || !size || (!out && capacity))
        return INFS_STATUS_INVALID_ARGUMENT;
    if(empty(root)) return INFS_STATUS_NOT_FOUND;
    struct node *n=calloc(1,sizeof(*n));
    if(!n) return INFS_STATUS_NO_MEMORY;
    struct infs20_tree_ref ref=*root;
    uint8_t lower_key[INFS20_TREE_KEY_MAX],upper_key[INFS20_TREE_KEY_MAX];
    struct bound lower={0},upper={0};
    int level=-1; infs_status st;
    for(;;) {
        st=read_node(io,&ref,n);
        if(!st) st=bounds(n,level,lower,upper);
        if(st) break;
        unsigned i=position(n,key,key_size);
        if(!n->level) {
            if(i==n->count || keycmp(n->entries[i].key,n->entries[i].key_size,key,key_size))
                st=INFS_STATUS_NOT_FOUND;
            else if(n->entries[i].value_size>capacity) st=INFS_STATUS_NO_SPACE;
            else {
                if(n->entries[i].value_size)
                    memcpy(out,n->entries[i].value,n->entries[i].value_size);
                *size=n->entries[i].value_size; st=INFS_STATUS_OK;
            }
            break;
        }
        i=route(n,key,key_size);
        ref_decode(n->entries[i].value,&ref);
        memcpy(lower_key,n->entries[i].key,n->entries[i].key_size);
        lower=(struct bound){lower_key,n->entries[i].key_size};
        if(i+1<n->count) {
            memcpy(upper_key,n->entries[i+1].key,n->entries[i+1].key_size);
            upper=(struct bound){upper_key,n->entries[i+1].key_size};
        }
        level=n->level-1;
    }
    free(n); return st;
}
static infs_status walk(const struct infs20_tree_io *io,const struct infs20_tree_ref *ref,
    int level,struct bound lower,struct bound upper,infs20_tree_visitor visit,
    infs20_tree_ref_visitor ref_visit,void *context)
{
    struct node *n=calloc(1,sizeof(*n));
    if(!n) return INFS_STATUS_NO_MEMORY;
    infs_status st=read_node(io,ref,n);
    if(!st) st=bounds(n,level,lower,upper);
    if(!st && ref_visit) st=ref_visit(context,ref);
    if(!st) for(unsigned i=0;i<n->count;++i) {
        const struct entry *e=n->entries+i;
        if(n->level) {
            struct infs20_tree_ref child; ref_decode(e->value,&child);
            st=walk(io,&child,n->level-1,entry_bound(e),
                    i+1<n->count?entry_bound(e+1):upper,visit,ref_visit,context);
        } else if(visit) st=visit(context,e->key,e->key_size,e->value,e->value_size);
        if(st) break;
    }
    free(n); return st;
}
infs_status infs20_tree_visit(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *root,infs20_tree_visitor visit,void *context)
{
    if(!io_valid(io,0) || !root) return INFS_STATUS_INVALID_ARGUMENT;
    if(empty(root)) return INFS_STATUS_OK;
    return walk(io,root,-1,(struct bound){0},(struct bound){0},visit,NULL,context);
}
/*
 * Range traversal uses the existing immutable membership proof. Child
 * intervals are [entry minimum, next minimum); prune those disjoint from
 * the requested interval before reading a child. Inherited upper bounds
 * remain mandatory even for the rightmost child. This avoids a full-tree
 * scan for an ownership family, a particular root's directory, or a
 * bounded extent-key range. An unvisited subtree is NOT verified.
 */
struct range_walk {
    struct bound start,end;
    int exclusive;
    size_t limit,seen;
    int more;
    infs20_tree_visitor visit;
    void *context;
};
static infs_status walk_range(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *ref,int level,struct bound lower,
    struct bound upper,struct range_walk *r)
{
    if((r->start.key && upper.key &&
        keycmp(upper.key,upper.size,r->start.key,r->start.size)<=0) ||
       (r->end.key && lower.key &&
        keycmp(lower.key,lower.size,r->end.key,r->end.size)>=0))
        return INFS_STATUS_OK;
    struct node *n=calloc(1,sizeof(*n));
    if(!n) return INFS_STATUS_NO_MEMORY;
    infs_status st=read_node(io,ref,n);
    if(!st) st=bounds(n,level,lower,upper);
    if(!st) {
        unsigned i=0;
        if(r->start.key)
            i=n->level ? route(n,r->start.key,r->start.size) :
                         position(n,r->start.key,r->start.size);
        if(!n->level && r->exclusive && r->start.key && i<n->count &&
           !keycmp(n->entries[i].key,n->entries[i].key_size,
                   r->start.key,r->start.size)) ++i;
        for(;i<n->count && !r->more;++i) {
            const struct entry *e=n->entries+i;
            if(r->end.key && keycmp(e->key,e->key_size,r->end.key,r->end.size)>=0)
                break;
            if(n->level) {
                struct infs20_tree_ref child;
                ref_decode(e->value,&child);
                st=walk_range(io,&child,n->level-1,entry_bound(e),
                    i+1<n->count?entry_bound(e+1):upper,r);
            } else if(r->seen==r->limit) {
                /* Prove there is another matching key, not just a
                 * potentially empty branch; continuation never guesses. */
                r->more=1;
            } else {
                st=r->visit(r->context,e->key,e->key_size,e->value,e->value_size);
                if(!st) ++r->seen;
            }
            if(st) break;
        }
    }
    free(n);
    return st;
}
infs_status infs20_tree_visit_range(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *root,const uint8_t *start,size_t start_size,
    int exclusive,const uint8_t *end,size_t end_size,size_t limit,
    infs20_tree_visitor visit,void *context,size_t *seen,int *more)
{
    if(!io_valid(io,0) || !root || !visit || !seen || !more || !limit ||
       (!start != !start_size) || (!end != !end_size) ||
       start_size>INFS20_TREE_KEY_MAX || end_size>INFS20_TREE_KEY_MAX ||
       (exclusive!=0 && exclusive!=1) || (exclusive && !start) ||
       (start && end && keycmp(start,start_size,end,end_size)>0))
        return INFS_STATUS_INVALID_ARGUMENT;
    struct range_walk r={{start,start_size},{end,end_size},exclusive,limit,
                         0,0,visit,context};
    infs_status st=INFS_STATUS_OK;
    if(!empty(root) && !(start && end &&
        !keycmp(start,start_size,end,end_size)))
        st=walk_range(io,root,-1,(struct bound){0},(struct bound){0},&r);
    if(!st) { *seen=r.seen; *more=r.more; }
    return st;
}
infs_status infs20_tree_visit_refs(const struct infs20_tree_io *io,
    const struct infs20_tree_ref *root,infs20_tree_ref_visitor visit,void *context)
{
    if(!io_valid(io,0) || !root || !visit) return INFS_STATUS_INVALID_ARGUMENT;
    if(empty(root)) return INFS_STATUS_OK;
    return walk(io,root,-1,(struct bound){0},(struct bound){0},NULL,visit,context);
}
