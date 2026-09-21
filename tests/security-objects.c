// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BLOCKS UINT64_C(4096)
#define BYTES ((size_t)(BLOCKS * INFS_BLOCK_SIZE))
struct image { uint8_t *p; size_t n; uint64_t rnd; };
static void ok(int x,const char*m){if(!x){fprintf(stderr,"security-objects: %s\n",m);exit(1);}}
static infs_status rd(void*c,uint64_t o,void*b,size_t n){struct image*i=c;if(o>i->n||n>i->n-(size_t)o)return INFS_STATUS_IO_ERROR;memcpy(b,i->p+(size_t)o,n);return INFS_STATUS_OK;}
static infs_status wr(void*c,uint64_t o,const void*b,size_t n){struct image*i=c;if(o>i->n||n>i->n-(size_t)o)return INFS_STATUS_IO_ERROR;memcpy(i->p+(size_t)o,b,n);return INFS_STATUS_OK;}
static infs_status fl(void*c){(void)c;return INFS_STATUS_OK;}
static infs_status sz(void*c,uint64_t*n,int*d){struct image*i=c;*n=i->n;*d=0;return INFS_STATUS_OK;}
static infs_status rn(void*c,void*b,size_t n){struct image*i=c;uint8_t*p=b;for(size_t x=0;x<n;++x){i->rnd^=i->rnd<<13;i->rnd^=i->rnd>>7;i->rnd^=i->rnd<<17;p[x]=(uint8_t)i->rnd;}return INFS_STATUS_OK;}
static infs_status tm(void*c,struct infs_timestamp*t){(void)c;t->seconds=1787288400;t->nanoseconds=0;return INFS_STATUS_OK;}
static void cl(void*c){(void)c;}
static const struct infs_storage_ops ops={
 .read_at=rd,.write_at=wr,.flush=fl,.get_size=sz,
 .random_bytes=rn,.current_time=tm,.close=cl
};
static struct infs_storage st(struct image*i){struct infs_storage s={&ops,i};return s;}

int main(void){
 struct image im={calloc(1,BYTES),BYTES,UINT64_C(0x123456789abcdef)};
 ok(im.p!=NULL,"allocate");
 struct infs_storage s=st(&im);
 ok(infs_format_storage(&s,"security-object-test")==INFS_STATUS_OK,"format");
 struct infs_volume v; s=st(&im);
 ok(infs_volume_open_storage(&v,&s,1)==INFS_STATUS_OK,"open");
 ok((infs_le64_to_cpu(v.sb.incompat_flags)&INFS_INCOMPAT_SECURITY_OBJECTS_V1)!=0,"feature enabled");
 ok(infs_create_file(&v,"/secured",NULL)==INFS_STATUS_OK,"create file");

 struct infs_security_principal p[2]={0};
 for(int i=0;i<16;i++){p[0].principal_id[i]=(uint8_t)(i+1);p[1].principal_id[i]=(uint8_t)(0x80+i);}
 p[0].kind=INFS_PRINCIPAL_USER;p[0].binding_type=INFS_BINDING_POSIX_UID;p[0].binding_size=4;
 p[0].binding[0]=0xe8;p[0].binding[1]=0x03;
 p[1].kind=INFS_PRINCIPAL_GROUP;p[1].binding_type=INFS_BINDING_WINDOWS_SID;p[1].binding_size=8;
 memcpy(p[1].binding,"SIDBYTES",8);
 struct infs_security_ace a[3]={0};
 memcpy(a[0].principal_id,p[0].principal_id,16);a[0].rights=INFS_RIGHT_READ_DATA|INFS_RIGHT_WRITE_DATA;a[0].disposition=INFS_ACE_ALLOW;
 memcpy(a[1].principal_id,p[1].principal_id,16);a[1].rights=INFS_RIGHT_DELETE;a[1].disposition=INFS_ACE_DENY;
 memcpy(a[2].principal_id,p[1].principal_id,16);a[2].rights=INFS_RIGHT_READ_ATTRIBUTES;a[2].disposition=INFS_ACE_ALLOW;a[2].flags=INFS_ACE_INHERIT_FILE;
 struct infs_security_descriptor d={p,2,a,3};
 ok(infs_set_security_descriptor(&v,"/secured",&d)==INFS_STATUS_OK,"set descriptor");

 struct infs_security_descriptor got={0};
 ok(infs_get_security_descriptor(&v,"/secured",&got)==INFS_STATUS_OK,"get descriptor");
 ok(got.principal_count==2&&got.ace_count==3,"counts");
 ok(got.aces[1].disposition==INFS_ACE_DENY&&got.aces[1].rights==INFS_RIGHT_DELETE,"ordered deny preserved");
 ok(got.principals[1].binding_type==INFS_BINDING_WINDOWS_SID&&got.principals[1].binding_size==8,"SID binding preserved");
 infs_free_security_descriptor(&got);

 struct infs_scrub_report report;
 ok(infs_scrub(&v,&report)==INFS_STATUS_OK&&report.metadata_errors==0,"scrub");
 infs_volume_close(&v); s=st(&im);
 ok(infs_volume_open_storage(&v,&s,1)==INFS_STATUS_OK,"remount");
 ok(infs_get_security_descriptor(&v,"/secured",&got)==INFS_STATUS_OK,"descriptor survives remount");
 infs_free_security_descriptor(&got);
 ok(infs_set_security_descriptor(&v,"/secured",NULL)==INFS_STATUS_OK,"remove descriptor");
 ok(infs_get_security_descriptor(&v,"/secured",&got)==INFS_STATUS_NOT_FOUND,"removed descriptor absent");
 ok(infs_scrub(&v,&report)==INFS_STATUS_OK&&report.metadata_errors==0,"scrub after removal");
 infs_volume_close(&v);free(im.p);puts("portable security objects: PASS");return 0;
}
