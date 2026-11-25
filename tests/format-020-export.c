// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "format20/volume.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"format20-export:%d: %s\n",__LINE__,#x); return 1; } } while(0)
enum { LANE=128, BLOCKS=3*LANE, DATA=100 };
static infs_status rd(void *o,uint64_t b,uint8_t *p)
{
    int fd=*(int *)o;
    return b<BLOCKS && pread(fd,p,INFS20_PAGE_BYTES,(off_t)(b*INFS20_PAGE_BYTES))==
        INFS20_PAGE_BYTES?0:INFS_STATUS_IO_ERROR;
}
static infs_status wr(void *o,uint64_t b,const uint8_t *p)
{
    int fd=*(int *)o;
    return b<BLOCKS && pwrite(fd,p,INFS20_PAGE_BYTES,(off_t)(b*INFS20_PAGE_BYTES))==
        INFS20_PAGE_BYTES?0:INFS_STATUS_IO_ERROR;
}
static infs_status fl(void *o) { return fsync(*(int *)o)?INFS_STATUS_IO_ERROR:0; }
static int run(const char *tool,const char *image,const char *out)
{
    pid_t pid=fork(); if(pid<0) return -1;
    if(!pid) { execl(tool,tool,image,out,(char *)NULL); _exit(127); }
    int status=0;
    return waitpid(pid,&status,0)==pid && WIFEXITED(status)?WEXITSTATUS(status):-1;
}
static int same(const char *path,const uint8_t *data,size_t n)
{
    int fd=open(path,O_RDONLY); if(fd<0) return 0;
    uint8_t out[2*INFS20_PAGE_BYTES]; struct stat s;
    int ok=!fstat(fd,&s) && (uint64_t)s.st_size==n &&
        n<=sizeof(out) && read(fd,out,n)==(ssize_t)n && !memcmp(out,data,n);
    close(fd); return ok;
}
static int contains(const char *path,const char *needle)
{
    FILE *f=fopen(path,"r"); if(!f) return 0;
    char line[1024]; int found=0;
    while(fgets(line,sizeof(line),f)) if(strstr(line,needle)) found=1;
    fclose(f); return found;
}
int main(int argc,char **argv)
{
    CHECK(argc==2);
    char temp[]="/tmp/infs20-export-XXXXXX"; CHECK(mkdtemp(temp));
    char image[256],complete[256],partial[256],path[512];
    snprintf(image,sizeof(image),"%s/image",temp);
    snprintf(complete,sizeof(complete),"%s/complete",temp);
    snprintf(partial,sizeof(partial),"%s/partial-out",temp);
    int fd=open(image,O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0 && !ftruncate(fd,(off_t)BLOCKS*INFS20_PAGE_BYTES));
    struct infs20_paged_layout l={{{1},{2},{3},BLOCKS,{0,LANE,2*LANE}},
        {20,LANE+20,2*LANE+20},{80,80,80}};
    struct infs20_store_io io={&fd,rd,wr,fl};
    uint8_t tx[16]={1},data[INFS20_PAGE_BYTES+53];
    for(size_t i=0;i<sizeof(data);++i) data[i]=(uint8_t)(i*13+7);
    CHECK(!infs20_paged_volume_create(&l,&io,tx));
    tx[0]=2; CHECK(!infs20_paged_volume_write(&l,&io,tx,"a",data,sizeof(data)));
    tx[0]=3; CHECK(!infs20_paged_volume_clone(&l,&io,tx,"a","b"));
    CHECK(!fl(&fd));
    struct stat before,after; CHECK(!fstat(fd,&before));
    CHECK(run(argv[1],image,complete)==0);
    CHECK(!fstat(fd,&after) && before.st_mtim.tv_sec==after.st_mtim.tv_sec &&
        before.st_mtim.tv_nsec==after.st_mtim.tv_nsec);
    snprintf(path,sizeof(path),"%s/active/a",complete); CHECK(same(path,data,sizeof(data)));
    snprintf(path,sizeof(path),"%s/active/b",complete); CHECK(same(path,data,sizeof(data)));
    snprintf(path,sizeof(path),"%s/manifest.jsonl",complete);
    CHECK(contains(path,"\"status\":\"complete\""));
    CHECK(run(argv[1],image,complete)==1);
    uint8_t block[INFS20_PAGE_BYTES]; CHECK(!rd(&fd,DATA+1,block));
    block[0]^=1; CHECK(!wr(&fd,DATA+1,block) && !fl(&fd));
    CHECK(run(argv[1],image,partial)==3);
    snprintf(path,sizeof(path),"%s/manifest.jsonl",partial);
    CHECK(contains(path,"\"status\":\"partial\"") &&
          contains(path,"\"missing_units\":1") && contains(path,"\"kind\":\"fragment\""));
    snprintf(path,sizeof(path),"%s/active/a",partial); CHECK(access(path,F_OK)!=0);
    snprintf(path,sizeof(path),"%s/active/b",partial); CHECK(access(path,F_OK)!=0);
    uint8_t verify[INFS20_PAGE_BYTES]; CHECK(!rd(&fd,DATA+1,verify) &&
        !memcmp(verify,block,sizeof(block)));
    /* Test-owned directory: remove only the generated files and known
     * fragment name read from its manifest. */
    snprintf(path,sizeof(path),"%s/manifest.jsonl",partial);
    FILE *manifest=fopen(path,"r"); CHECK(manifest);
    char line[1024],fragment[128]={0};
    while(fgets(line,sizeof(line),manifest)) {
        char *p=strstr(line,"\"path\":\"partial/");
        if(!p) continue;
        p+=8; char *end=strchr(p,'"');
        if(end && (size_t)(end-p)<sizeof(fragment)) {
            memcpy(fragment,p,(size_t)(end-p)); fragment[end-p]=0;
        }
    }
    fclose(manifest);
    CHECK(!strncmp(fragment,"partial/",8));
    snprintf(path,sizeof(path),"%s/%s",partial,fragment); CHECK(same(path,data,INFS20_PAGE_BYTES));
    CHECK(!unlink(path));
    snprintf(path,sizeof(path),"%s/partial",partial); CHECK(!rmdir(path));
    snprintf(path,sizeof(path),"%s/manifest.jsonl",partial); CHECK(!unlink(path));
    CHECK(!rmdir(partial));
    snprintf(path,sizeof(path),"%s/active/a",complete); CHECK(!unlink(path));
    snprintf(path,sizeof(path),"%s/active/b",complete); CHECK(!unlink(path));
    snprintf(path,sizeof(path),"%s/active",complete); CHECK(!rmdir(path));
    snprintf(path,sizeof(path),"%s/manifest.jsonl",complete); CHECK(!unlink(path));
    CHECK(!rmdir(complete));
    /* Reconstruct a real sparse file from independent physical allocations. */
    CHECK(!close(fd) && !unlink(image));
    fd=open(image,O_RDWR|O_CREAT|O_EXCL,0600);
    CHECK(fd>=0 && !ftruncate(fd,(off_t)BLOCKS*INFS20_PAGE_BYTES));
    memset(tx,0,sizeof(tx)); tx[0]=11;
    CHECK(!infs20_paged_volume_create(&l,&io,tx));
    tx[0]=12; CHECK(!infs20_paged_volume_write(&l,&io,tx,"sparse",NULL,0));
    tx[0]=13; CHECK(!infs20_paged_volume_write_range(&l,&io,tx,"sparse",3,data,17));
    tx[0]=14; CHECK(!infs20_paged_volume_write_range(&l,&io,tx,"sparse",4099,data+50,11));
    CHECK(run(argv[1],image,complete)==0);
    uint8_t wanted[4110]={0};
    memcpy(wanted+3,data,17); memcpy(wanted+4099,data+50,11);
    snprintf(path,sizeof(path),"%s/active/sparse",complete);
    CHECK(same(path,wanted,sizeof(wanted)) && !unlink(path));
    snprintf(path,sizeof(path),"%s/active",complete); CHECK(!rmdir(path));
    snprintf(path,sizeof(path),"%s/manifest.jsonl",complete);
    CHECK(contains(path,"\"kind\":\"extent\"") && !unlink(path));
    CHECK(!rmdir(complete));
    /* A partial export may expose only mapped portions of a good unit.
     * The clipped file retains bytes [100,150) and its second unit. */
    tx[0]=15; CHECK(!infs20_paged_volume_write(&l,&io,tx,"clip",data,sizeof(data)));
    tx[0]=16; CHECK(!infs20_paged_volume_punch(&l,&io,tx,"clip",0,100));
    tx[0]=17; CHECK(!infs20_paged_volume_punch(&l,&io,tx,"clip",150,INFS20_PAGE_BYTES-150));
    CHECK(!rd(&fd,DATA+3,block));
    block[0]^=1; CHECK(!wr(&fd,DATA+3,block) && !fl(&fd));
    CHECK(run(argv[1],image,partial)==3);
    snprintf(path,sizeof(path),"%s/active/sparse",partial);
    CHECK(same(path,wanted,sizeof(wanted)) && !unlink(path));
    snprintf(path,sizeof(path),"%s/active/clip",partial); CHECK(access(path,F_OK)!=0);
    snprintf(path,sizeof(path),"%s/active",partial); CHECK(!rmdir(path));
    snprintf(path,sizeof(path),"%s/manifest.jsonl",partial);
    CHECK(contains(path,"\"missing_units\":1") &&
          contains(path,"\"offset\":100,\"size\":50,\"path\":\"partial/"));
    manifest=fopen(path,"r"); CHECK(manifest);
    memset(fragment,0,sizeof(fragment)); unsigned fragments=0;
    while(fgets(line,sizeof(line),manifest)) {
        char *p=strstr(line,"\"path\":\"partial/");
        if(!p) continue;
        p+=8; char *end=strchr(p,'"');
        CHECK(end && (size_t)(end-p)<sizeof(fragment));
        memcpy(fragment,p,(size_t)(end-p)); fragment[end-p]=0; ++fragments;
    }
    fclose(manifest);
    CHECK(fragments==1 && !strncmp(fragment,"partial/",8));
    snprintf(path,sizeof(path),"%s/%s",partial,fragment);
    CHECK(same(path,data+100,50) && !unlink(path));
    snprintf(path,sizeof(path),"%s/partial",partial); CHECK(!rmdir(path));
    snprintf(path,sizeof(path),"%s/manifest.jsonl",partial); CHECK(!unlink(path));
    CHECK(!rmdir(partial) && !close(fd) && !unlink(image) && !rmdir(temp));
    puts("format20-export: PASS (complete named files, partial verified fragments, source immutability)");
    return 0;
}
