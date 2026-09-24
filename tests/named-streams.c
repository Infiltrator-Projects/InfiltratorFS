// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE (64u * 1024u * 1024u)

struct mem { uint8_t *p; size_t n; uint64_t r; };
static infs_status rd(void *c,uint64_t o,void *b,size_t n){struct mem*m=c;if(o>m->n||n>m->n-(size_t)o)return INFS_STATUS_IO_ERROR;memcpy(b,m->p+(size_t)o,n);return INFS_STATUS_OK;}
static infs_status wr(void *c,uint64_t o,const void *b,size_t n){struct mem*m=c;if(o>m->n||n>m->n-(size_t)o)return INFS_STATUS_IO_ERROR;memcpy(m->p+(size_t)o,b,n);return INFS_STATUS_OK;}
static infs_status fl(void *c){(void)c;return INFS_STATUS_OK;}
static infs_status sz(void *c,uint64_t*n,int*d){struct mem*m=c;*n=m->n;*d=0;return INFS_STATUS_OK;}
static infs_status rn(void *c,void *b,size_t n){struct mem*m=c;uint8_t*out=b;for(size_t i=0;i<n;i++){m->r^=m->r<<13;m->r^=m->r>>7;m->r^=m->r<<17;out[i]=(uint8_t)m->r;}return INFS_STATUS_OK;}
static infs_status tm(void *c,struct infs_timestamp*t){(void)c;t->seconds=1786748400;t->nanoseconds=123456789;return INFS_STATUS_OK;}
static void cl(void *c){(void)c;}
static const struct infs_storage_ops ops={.read_at=rd,.write_at=wr,.flush=fl,.get_size=sz,.random_bytes=rn,.current_time=tm,.close=cl};
static struct infs_storage sto(struct mem*m){struct infs_storage s={.ops=&ops,.context=m};return s;}
static void ok(int x,const char*m){if(!x){fprintf(stderr,"named-streams: %s\n",m);exit(1);}}

int main(void)
{
    struct mem m={calloc(1,IMAGE_SIZE),IMAGE_SIZE,UINT64_C(0x1f2e3d4c5b6a7988)};
    ok(m.p!=NULL,"allocate");
    struct infs_storage s=sto(&m);
    ok(infs_format_storage(&s,"named-streams")==INFS_STATUS_OK,"format");

    struct infs_volume v;
    s=sto(&m);
    ok(infs_volume_open_storage(&v,&s,1)==INFS_STATUS_OK,"open");
    ok((infs_le64_to_cpu(v.sb.incompat_flags)&INFS_INCOMPAT_NAMED_STREAMS_V1)!=0,"feature bit");

    struct infs_create_options co={.posix_permissions=0644};
    ok(infs_create_file(&v,"/owner",&co)==INFS_STATUS_OK,"create owner");

    const uint8_t type_id[16]={'I','N','F','S','-','M','E','T','A','-','T','E','S','T','0','1'};
    const uint8_t ext_bytes[]={0x41,0x00,0x42,0xff};
    struct infs_typed_extension ext={0};
    memcpy(ext.type_id,type_id,16); ext.type_version=1;
    ext.flags=INFS_EXTENSION_FLAG_PRESERVE_OPAQUE;
    ext.data=(uint8_t*)ext_bytes; ext.data_size=sizeof(ext_bytes);
    ok(infs_set_typed_extension(&v,"/owner",&ext)==INFS_STATUS_OK,"attach typed extension");

    size_t big_n=INFS_BLOCK_SIZE*5u+731u;
    uint8_t *big=malloc(big_n),*readback=malloc(big_n);
    ok(big&&readback,"allocate stream buffers");
    for(size_t i=0;i<big_n;i++) big[i]=(uint8_t)((i*37u+11u)&0xffu);

    infs_status stream_status =
        infs_named_stream_set(&v, "/owner", "user.preview/data", big, big_n);
    if (stream_status != INFS_STATUS_OK) {
        fprintf(stderr, "named-streams: set large stream: %s (%d)\n",
                infs_status_string(stream_status), (int)stream_status);
        exit(1);
    }
    memset(readback,0,big_n);
    ok(infs_named_stream_read(&v,"/owner","user.preview/data",readback,big_n,0)==(int64_t)big_n,"read large stream");
    ok(!memcmp(big,readback,big_n),"large stream roundtrip");

    const char small[]="replacement-metadata";
    ok(infs_named_stream_set(&v,"/owner","user.preview/data",small,sizeof(small))==INFS_STATUS_OK,"replace stream");
    char small_out[sizeof(small)]={0};
    ok(infs_named_stream_read(&v,"/owner","user.preview/data",small_out,sizeof(small_out),0)==(int64_t)sizeof(small),"read replacement");
    ok(!memcmp(small,small_out,sizeof(small)),"replacement roundtrip");

    const uint8_t second[]={1,2,3,4,5,6,7,8,9};
    ok(infs_named_stream_set(&v,"/owner","com.infiltrator.tag",second,sizeof(second))==INFS_STATUS_OK,"set second stream");

    struct infs_named_stream_info *items=NULL; size_t count=0;
    ok(infs_named_stream_list(&v,"/owner",&items,&count)==INFS_STATUS_OK&&count==2,"list two streams");
    int saw_preview=0,saw_tag=0;
    for(size_t i=0;i<count;i++){
        if(!strcmp(items[i].name,"user.preview/data")&&items[i].logical_size==sizeof(small))saw_preview=1;
        if(!strcmp(items[i].name,"com.infiltrator.tag")&&items[i].logical_size==sizeof(second))saw_tag=1;
    }
    ok(saw_preview&&saw_tag,"list names and sizes");
    infs_free_named_stream_infos(items);

    struct infs_typed_extension ext_out={0};
    ok(infs_get_typed_extension(&v,"/owner",&ext_out)==INFS_STATUS_OK,"typed extension survives stream indirection");
    ok(ext_out.data_size==sizeof(ext_bytes)&&!memcmp(ext_out.data,ext_bytes,sizeof(ext_bytes)),"typed extension data preserved");
    infs_free_typed_extension(&ext_out);

    struct infs_scrub_report report;
    ok(infs_scrub(&v,&report)==INFS_STATUS_OK&&report.metadata_errors==0&&report.checksum_errors==0,"scrub with streams");
    ok(infs_volume_sync(&v)==INFS_STATUS_OK,"sync");
    infs_volume_close(&v);

    s=sto(&m);
    ok(infs_volume_open_storage(&v,&s,1)==INFS_STATUS_OK,"reopen");
    memset(small_out,0,sizeof(small_out));
    ok(infs_named_stream_read(&v,"/owner","user.preview/data",small_out,sizeof(small_out),0)==(int64_t)sizeof(small),"stream survives reopen");
    ok(!memcmp(small,small_out,sizeof(small)),"reopen content");
    ok(infs_get_typed_extension(&v,"/owner",&ext_out)==INFS_STATUS_OK,"extension survives reopen");
    infs_free_typed_extension(&ext_out);

    ok(infs_named_stream_delete(&v,"/owner","user.preview/data")==INFS_STATUS_OK,"delete first stream");
    ok(infs_named_stream_read(&v,"/owner","user.preview/data",small_out,sizeof(small_out),0)==INFS_STATUS_NOT_FOUND,"deleted stream absent");
    ok(infs_named_stream_delete(&v,"/owner","com.infiltrator.tag")==INFS_STATUS_OK,"delete final stream");
    items=NULL;count=99;
    ok(infs_named_stream_list(&v,"/owner",&items,&count)==INFS_STATUS_OK&&count==0&&items==NULL,"empty stream list");
    ok(infs_get_typed_extension(&v,"/owner",&ext_out)==INFS_STATUS_OK,"typed extension retained after final stream deletion");
    infs_free_typed_extension(&ext_out);
    ok(infs_scrub(&v,&report)==INFS_STATUS_OK&&report.metadata_errors==0&&report.checksum_errors==0,"scrub after deletion");

    infs_volume_close(&v);
    free(readback); free(big); free(m.p);
    puts("named-streams: PASS");
    return 0;
}
