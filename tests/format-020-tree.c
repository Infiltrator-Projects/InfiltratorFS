// SPDX-License-Identifier: GPL-3.0-or-later
#include "format20/tree.h"
#include "infilfs/endian.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-tree:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { LANE=2048, BLOCKS=3*LANE, KEY=900, VALUE=512, COUNT=96 };
struct medium {
    uint8_t *bytes;
    unsigned next,writes,reads,fail;
};
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    struct medium *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    ++m->reads; memcpy(p,m->bytes+b*INFS20_PAGE_BYTES,INFS20_PAGE_BYTES);
    return INFS_STATUS_OK;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    struct medium *m=o;
    if(b>=BLOCKS) return INFS_STATUS_IO_ERROR;
    if(++m->writes==m->fail) return INFS_STATUS_IO_ERROR;
    memcpy(m->bytes+b*INFS20_PAGE_BYTES,p,INFS20_PAGE_BYTES);
    return INFS_STATUS_OK;
}
static infs_status reserve(void *o,uint8_t id[16],uint64_t blocks[3])
{
    struct medium *m=o;
    if(m->next==LANE) return INFS_STATUS_NO_SPACE;
    unsigned n=m->next++;
    memset(id,0,16); infs_store_le64(id,(uint64_t)n+1); id[15]=0x71;
    for(unsigned i=0;i<3;++i) blocks[i]=(uint64_t)i*LANE+n;
    return INFS_STATUS_OK;
}
static void key(unsigned i,uint8_t k[KEY])
{
    memset(k,(int)(i%251),KEY);
    k[0]=(uint8_t)(i>>8); k[1]=(uint8_t)i;
}
static void value(unsigned i,uint8_t v[VALUE])
{
    for(unsigned j=0;j<VALUE;++j) v[j]=(uint8_t)(i*19+j*13);
}
static int expect(const struct infs20_tree_io *io,const struct infs20_tree_ref *r,unsigned i)
{
    uint8_t k[KEY],v[VALUE],out[VALUE]; size_t n=0;
    key(i,k); value(i,v);
    return !infs20_tree_get(io,r,k,sizeof(k),out,sizeof(out),&n) &&
           n==sizeof(v) && !memcmp(v,out,n);
}
struct traversal { unsigned next,seen; };
static infs_status visit(void *o,const uint8_t *k,size_t ks,const uint8_t *v,size_t vs)
{
    struct traversal *t=o;
    uint8_t expected[KEY],data[VALUE]; key(t->next,expected); value(t->next,data);
    if(ks!=KEY || vs!=VALUE || memcmp(k,expected,KEY) || memcmp(v,data,VALUE))
        return INFS_STATUS_CORRUPT;
    ++t->next; ++t->seen; return INFS_STATUS_OK;
}
int main(void)
{
    struct medium m={0};
    m.bytes=calloc(BLOCKS,INFS20_PAGE_BYTES); CHECK(m.bytes);
    struct infs20_tree_io io={&m,{1},{2},{3},BLOCKS,1,rd,wr,reserve};
    struct infs20_tree_ref root={0},retained={0},next;
    uint8_t k[KEY],v[VALUE],out[VALUE];
    /* Descending order forces minimum-key changes along entire paths.
     * Long keys force multiple levels with a modest record population. */
    for(unsigned n=COUNT;n>0;--n) {
        unsigned i=n-1; key(i,k); value(i,v); ++io.sequence;
        CHECK(!infs20_tree_put(&io,&root,k,KEY,v,VALUE,&next));
        root=next;
        if(n==COUNT/2) retained=root;
    }
    CHECK(!infs20_tree_visit(&io,&root,NULL,NULL));
    struct traversal t={0}; CHECK(!infs20_tree_visit(&io,&root,visit,&t));
    CHECK(t.seen==COUNT);
    for(unsigned i=0;i<COUNT;++i) CHECK(expect(&io,&root,i));
    CHECK(expect(&io,&retained,COUNT-1));
    key(0,k); size_t size=99; memset(out,0xa5,sizeof(out));
    CHECK(infs20_tree_get(&io,&retained,k,KEY,out,VALUE,&size)==INFS_STATUS_NOT_FOUND);
    CHECK(size==99 && out[0]==0xa5);
    uint8_t *root_page=m.bytes+root.blocks[0]*INFS20_PAGE_BYTES;
    unsigned level=infs_load_le16(root_page+INFS20_OFF_PAYLOAD+4);
    CHECK(level>=2);
    struct infs20_tree_ref original=root;
    key(COUNT/2,k); memset(v,0x87,VALUE); ++io.sequence;
    unsigned before=m.writes;
    CHECK(!infs20_tree_put(&io,&root,k,KEY,v,VALUE,&next));
    CHECK(m.writes-before==3*(level+1));
    root=next;
    CHECK(expect(&io,&original,COUNT/2));
    CHECK(!infs20_tree_get(&io,&root,k,KEY,out,VALUE,&size) &&
          size==VALUE && !memcmp(out,v,VALUE));
    /* Every private write failure must leave the prior root readable. */
    unsigned mutation_writes=m.writes-before;
    for(unsigned cut=1;cut<=mutation_writes;++cut) {
        ++io.sequence; m.fail=m.writes+cut;
        memset(&next,0xa5,sizeof(next));
        uint8_t prior[sizeof(next)]; memcpy(prior,&next,sizeof(prior));
        CHECK(infs20_tree_put(&io,&original,k,KEY,v,VALUE,&next)==INFS_STATUS_IO_ERROR);
        CHECK(!memcmp(prior,&next,sizeof(prior)));
        m.fail=0;
        CHECK(expect(&io,&original,COUNT/2));
    }
    /* Damage different physical copies at different levels; committed
     * child references, rather than a newest-record scan, choose data. */
    uint8_t *p=m.bytes+original.blocks[0]*INFS20_PAGE_BYTES;
    const uint8_t *entry=p+INFS20_OFF_PAYLOAD+INFS20_TREE_NODE_HEADER;
    uint16_t ks=infs_load_le16(entry);
    uint64_t child_lane1=infs_load_le64(entry+4+ks+64);
    memset(p,0,INFS20_PAGE_BYTES);
    memset(m.bytes+child_lane1*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    CHECK(!infs20_tree_visit(&io,&original,NULL,NULL));
    CHECK(expect(&io,&original,0));
    /* Delete all keys from a fresh retained view, pruning empty children.
     * No metadata page of the original view is overwritten. */
    root=original;
    for(unsigned i=0;i<COUNT;++i) {
        key(i,k); ++io.sequence;
        CHECK(!infs20_tree_remove(&io,&root,k,KEY,&next)); root=next;
        CHECK(!infs20_tree_visit(&io,&root,NULL,NULL));
    }
    CHECK(!root.sequence);
    CHECK(expect(&io,&original,COUNT-1));
    key(0,k); before=m.writes;
    CHECK(infs20_tree_remove(&io,&root,k,KEY,&next)==INFS_STATUS_NOT_FOUND &&
          m.writes==before);
    /* Oversized entries fail without writing. */
    uint8_t huge[INFS20_PAYLOAD_BYTES]={0};
    CHECK(infs20_tree_put(&io,&root,k,KEY,huge,sizeof(huge),&next)==INFS_STATUS_NO_SPACE &&
          m.writes==before);
    /* No matching root proof remains. No value is exposed. */
    for(unsigned lane=0;lane<3;++lane)
        memset(m.bytes+original.blocks[lane]*INFS20_PAGE_BYTES,0,INFS20_PAGE_BYTES);
    memset(out,0xa5,sizeof(out)); size=99;
    CHECK(infs20_tree_get(&io,&original,k,KEY,out,VALUE,&size)==INFS_STATUS_CORRUPT);
    CHECK(size==99 && out[0]==0xa5);
    free(m.bytes);
    printf("format20-tree: PASS (%u keys, initial height %u, %u private-write failure cases)\n",
           COUNT,level+1,mutation_writes);
    return 0;
}
