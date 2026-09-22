// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format.h"
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define IMAGE_SIZE (64u * 1024u * 1024u)
struct mem{uint8_t*p;size_t n;uint64_t r;};
static infs_status rd(void*c,uint64_t o,void*b,size_t n){struct mem*m=c;if(o>m->n||n>m->n-(size_t)o)return INFS_STATUS_IO_ERROR;memcpy(b,m->p+(size_t)o,n);return 0;}
static infs_status wr(void*c,uint64_t o,const void*b,size_t n){struct mem*m=c;if(o>m->n||n>m->n-(size_t)o)return INFS_STATUS_IO_ERROR;memcpy(m->p+(size_t)o,b,n);return 0;}
static infs_status fl(void*c){(void)c;return 0;}
static infs_status sz(void*c,uint64_t*n,int*d){struct mem*m=c;*n=m->n;*d=0;return 0;}
static infs_status rn(void*c,void*b,size_t n){struct mem*m=c;uint8_t*out=b;for(size_t i=0;i<n;i++){m->r^=m->r<<13;m->r^=m->r>>7;m->r^=m->r<<17;out[i]=(uint8_t)m->r;}return 0;}
static infs_status tm(void*c,struct infs_timestamp*t){(void)c;t->seconds=1786748400;t->nanoseconds=0;return 0;}
static void cl(void*c){(void)c;}
static const struct infs_storage_ops ops={.read_at=rd,.write_at=wr,.flush=fl,.get_size=sz,.random_bytes=rn,.current_time=tm,.close=cl};
static struct infs_storage sto(struct mem*m){struct infs_storage s={.ops=&ops,.context=m};return s;}
static void ok(int x,const char*m){if(!x){fprintf(stderr,"typed-extension: %s\n",m);exit(1);}}
int main(void){
 struct mem m={calloc(1,IMAGE_SIZE),IMAGE_SIZE,0x123456789abcdef0ULL};ok(m.p!=NULL,"allocate");
 struct infs_storage s=sto(&m);ok(infs_format_storage(&s,"typed-extension")==INFS_STATUS_OK,"format");
 s=sto(&m);struct infs_volume v;ok(infs_volume_open_storage(&v,&s,1)==INFS_STATUS_OK,"open");
 ok((infs_le64_to_cpu(v.sb.incompat_flags)&INFS_INCOMPAT_TYPED_EXTENSIONS)!=0,"feature");
 struct infs_create_options co={.posix_permissions=0644};ok(infs_create_file(&v,"/source",&co)==INFS_STATUS_OK,"create");
 struct infs_typed_extension in={0};const uint8_t tid[16]={0x49,0x4e,0x46,0x53,0x2d,0x52,0x45,0x50,0x41,0x52,0x53,0x45,0x30,0x30,0x30,0x31};
 const uint8_t payload[]={0x10,0x20,0x30,0x00,0xff,0x7f};memcpy(in.type_id,tid,16);in.type_version=3;in.flags=INFS_EXTENSION_FLAG_REPARSE|INFS_EXTENSION_FLAG_PRESERVE_OPAQUE;in.data=(uint8_t*)payload;in.data_size=sizeof(payload);
 ok(infs_set_typed_extension(&v,"/source",&in)==INFS_STATUS_OK,"attach");
 struct infs_typed_extension out={0};ok(infs_get_typed_extension(&v,"/source",&out)==INFS_STATUS_OK,"get");
 ok(!memcmp(out.type_id,tid,16)&&out.type_version==3&&out.flags==in.flags&&out.data_size==sizeof(payload)&&!memcmp(out.data,payload,sizeof(payload)),"roundtrip");infs_free_typed_extension(&out);
 struct infs_attributes a,b;uint8_t zero[16]={0};ok(infs_get_attributes(&v,"/source",&a)==INFS_STATUS_OK&&memcmp(a.extended_attributes_object_id,zero,16)!=0,"attached id");
 ok(infs_reflink_file(&v,"/source","/clone")==INFS_STATUS_OK,"reflink");ok(infs_get_attributes(&v,"/clone",&b)==INFS_STATUS_OK&&!memcmp(a.extended_attributes_object_id,b.extended_attributes_object_id,16),"reflink attachment");
 struct infs_scrub_report report;ok(infs_scrub(&v,&report)==INFS_STATUS_OK&&report.metadata_errors==0,"scrub");ok(infs_volume_sync(&v)==INFS_STATUS_OK,"sync");infs_volume_close(&v);
 s=sto(&m);ok(infs_volume_open_storage(&v,&s,1)==INFS_STATUS_OK,"reopen");ok(infs_get_typed_extension(&v,"/clone",&out)==INFS_STATUS_OK&&out.data_size==sizeof(payload)&&!memcmp(out.data,payload,sizeof(payload)),"reopen");infs_free_typed_extension(&out);
 ok(infs_set_typed_extension(&v,"/source",NULL)==INFS_STATUS_OK,"detach");ok(infs_get_typed_extension(&v,"/source",&out)==INFS_STATUS_NOT_FOUND,"detached");ok(infs_get_typed_extension(&v,"/clone",&out)==INFS_STATUS_OK,"clone retained");infs_free_typed_extension(&out);
 infs_volume_close(&v);free(m.p);puts("typed-extension: PASS");return 0;
}
