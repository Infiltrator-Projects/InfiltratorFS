// SPDX-License-Identifier: GPL-3.0-or-later
#define _POSIX_C_SOURCE 200809L
#define _FILE_OFFSET_BITS 64
#include "format20/volume.h"
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Experimental offline exporter. Only regular, read-only source images
 * and newly created output directories are admitted. The recovery visitor
 * proves metadata first, then supplies verified allocation units once.
 * Never fill missing data with zeros or publish a misleading complete file.
 * This is intentionally not installed as a production unformatter.
 */
struct source { int fd; struct stat initial; };
struct mapping {
    uint8_t allocation[16];
    uint64_t pin;
    size_t size;
    char name[INFS20_VOLUME_NAME_MAX+1];
};
struct export {
    int dir;
    FILE *manifest;
    struct mapping *maps;
    size_t count,capacity,units,size,complete,partial,bad;
    uint8_t allocation[16], data[INFS20_VOLUME_BYTES];
    uint32_t good;
    int active;
};
static infs_status read_at(void *opaque,uint64_t offset,uint8_t *p,size_t n)
{
    struct source *s=opaque;
    uint64_t size=(uint64_t)s->initial.st_size;
    if(offset>size || n>size-offset) return INFS_STATUS_IO_ERROR;
    while(n) {
        ssize_t got=pread(s->fd,p,n,(off_t)offset);
        if(got<0 && errno==EINTR) continue;
        if(got<=0) return INFS_STATUS_IO_ERROR;
        p+=got; n-=(size_t)got; offset+=(uint64_t)got;
    }
    return INFS_STATUS_OK;
}
static int unchanged(const struct source *s)
{
    struct stat now;
    return !fstat(s->fd,&now) && now.st_dev==s->initial.st_dev &&
        now.st_ino==s->initial.st_ino && now.st_size==s->initial.st_size &&
        now.st_mtim.tv_sec==s->initial.st_mtim.tv_sec &&
        now.st_mtim.tv_nsec==s->initial.st_mtim.tv_nsec &&
        now.st_ctim.tv_sec==s->initial.st_ctim.tv_sec &&
        now.st_ctim.tv_nsec==s->initial.st_ctim.tv_nsec;
}
static void hex(const uint8_t *p,size_t n,char *out)
{
    static const char digits[]="0123456789abcdef";
    for(size_t i=0;i<n;++i) {
        out[2*i]=digits[p[i]>>4]; out[2*i+1]=digits[p[i]&15];
    }
    out[2*n]=0;
}
static void json_string(FILE *f,const char *s)
{
    fputc('"',f);
    for(;*s;++s) {
        unsigned char c=(unsigned char)*s;
        if(c=='"' || c=='\\') { fputc('\\',f); fputc(c,f); }
        else if(c<32) fprintf(f,"\\u%04x",(unsigned)c);
        else fputc(c,f);
    }
    fputc('"',f);
}
static int subdir(int root,const char *name)
{
    if(mkdirat(root,name,0700) && errno!=EEXIST) return -1;
    return openat(root,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
}
static int file_write(int dir,const char *name,const uint8_t *data,size_t n)
{
    int fd=openat(dir,name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) return -1;
    int failed=0;
    while(n) {
        ssize_t put=write(fd,data,n);
        if(put<0 && errno==EINTR) continue;
        if(put<=0) { failed=1; break; }
        data+=put; n-=(size_t)put;
    }
    if(!failed && fsync(fd)) failed=1;
    if(close(fd)) failed=1;
    if(failed) { (void)unlinkat(dir,name,0); return -1; }
    return fsync(dir);
}
static infs_status mapping(void *opaque,const struct infs20_recovery_mapping *m)
{
    struct export *e=opaque;
    size_t len=strlen(m->name);
    /* Current model names are flat validated components. Recheck at the
     * filesystem boundary; never interpret names as destination paths. */
    if(!len || len>INFS20_VOLUME_NAME_MAX || strchr(m->name,'/') ||
       strchr(m->name,'\\') || !strcmp(m->name,".") || !strcmp(m->name,"..") ||
       m->size>INFS20_VOLUME_BYTES) return INFS_STATUS_CORRUPT;
    if(e->count==e->capacity) {
        size_t cap=e->capacity?e->capacity*2:16;
        if(cap>INFS20_VOLUME_FILES*INFS20_MODEL_EDGES)
            cap=INFS20_VOLUME_FILES*INFS20_MODEL_EDGES;
        if(cap<=e->count) return INFS_STATUS_NO_SPACE;
        struct mapping *next=realloc(e->maps,cap*sizeof(*next));
        if(!next) return INFS_STATUS_NO_MEMORY;
        e->maps=next; e->capacity=cap;
    }
    struct mapping *item=e->maps+e->count++;
    memcpy(item->allocation,m->allocation_id,16);
    item->pin=m->pin; item->size=m->size; memcpy(item->name,m->name,len+1);
    char root[33],allocation[33]; hex(m->root_id,16,root); hex(m->allocation_id,16,allocation);
    fprintf(e->manifest,"{\"kind\":\"mapping\",\"root\":\"%s\",\"allocation\":\"%s\",\"pin\":%" PRIu64 ",\"role\":%u,\"size\":%zu,\"name\":",
            root,allocation,m->pin,(unsigned)m->role,m->size);
    json_string(e->manifest,m->name); fputs("}\n",e->manifest);
    return ferror(e->manifest)?INFS_STATUS_IO_ERROR:INFS_STATUS_OK;
}
static infs_status finish_allocation(struct export *e)
{
    if(!e->active) return INFS_STATUS_OK;
    size_t expected=e->size?1+(e->size-1)/INFS20_PAGE_BYTES:1;
    if(e->units!=expected || expected>INFS20_VOLUME_UNITS) return INFS_STATUS_CORRUPT;
    uint32_t mask=(UINT32_C(1)<<expected)-1;
    char id[33]; hex(e->allocation,16,id);
    if(e->good==mask) {
        for(size_t i=0;i<e->count;++i) {
            const struct mapping *m=e->maps+i;
            if(memcmp(m->allocation,e->allocation,16)) continue;
            char dir[48],path[128];
            if(m->pin) snprintf(dir,sizeof(dir),"pin-%" PRIu64,m->pin);
            else strcpy(dir,"active");
            int fd=subdir(e->dir,dir);
            if(fd<0) return INFS_STATUS_IO_ERROR;
            int result=file_write(fd,m->name,e->data,e->size);
            if(close(fd)) result=-1;
            if(result) return INFS_STATUS_IO_ERROR;
            snprintf(path,sizeof(path),"%s/%s",dir,m->name);
            fprintf(e->manifest,"{\"kind\":\"file\",\"allocation\":\"%s\",\"path\":",id);
            json_string(e->manifest,path); fputs("}\n",e->manifest);
            ++e->complete;
        }
    } else {
        for(size_t i=0;i<e->count;++i)
            if(!memcmp(e->maps[i].allocation,e->allocation,16)) ++e->partial;
        int fd=subdir(e->dir,"partial");
        if(fd<0) return INFS_STATUS_IO_ERROR;
        int result=0;
        for(size_t i=0;i<expected;++i) {
            if(!(e->good&(UINT32_C(1)<<i))) continue;
            size_t off=i*INFS20_PAGE_BYTES,n=e->size>off?e->size-off:0;
            if(n>INFS20_PAGE_BYTES) n=INFS20_PAGE_BYTES;
            char name[80]; snprintf(name,sizeof(name),"%s-%zu.bin",id,off);
            if(file_write(fd,name,e->data+off,n)) { result=-1; break; }
            fprintf(e->manifest,"{\"kind\":\"fragment\",\"allocation\":\"%s\",\"offset\":%zu,\"size\":%zu,\"path\":\"partial/%s\"}\n",
                    id,off,n,name);
        }
        if(close(fd)) result=-1;
        if(result) return INFS_STATUS_IO_ERROR;
    }
    e->active=0;
    return ferror(e->manifest)?INFS_STATUS_IO_ERROR:INFS_STATUS_OK;
}
static infs_status unit(void *opaque,const struct infs20_recovery_unit *u)
{
    struct export *e=opaque;
    if(!e->active || memcmp(e->allocation,u->allocation_id,16)) {
        infs_status st=finish_allocation(e); if(st) return st;
        int found=0;
        for(size_t i=0;i<e->count;++i) {
            const struct mapping *m=e->maps+i;
            if(memcmp(m->allocation,u->allocation_id,16)) continue;
            if(found && e->size!=m->size) return INFS_STATUS_CORRUPT;
            found=1; e->size=m->size;
        }
        if(!found) return INFS_STATUS_CORRUPT;
        memcpy(e->allocation,u->allocation_id,16);
        e->units=0; e->good=0; e->active=1;
    }
    size_t off=e->units*INFS20_PAGE_BYTES,n=e->size>off?e->size-off:0;
    if(n>INFS20_PAGE_BYTES) n=INFS20_PAGE_BYTES;
    if(e->units>=INFS20_VOLUME_UNITS || u->logical_offset!=off ||
       u->logical_size!=n || (u->status==0)!=!!u->data)
        return INFS_STATUS_CORRUPT;
    if(!u->status) {
        if(n) memcpy(e->data+off,u->data,n);
        e->good|=UINT32_C(1)<<e->units;
    } else ++e->bad;
    ++e->units;
    char id[33]; hex(u->allocation_id,16,id);
    fprintf(e->manifest,"{\"kind\":\"unit\",\"allocation\":\"%s\",\"offset\":%zu,\"size\":%zu,\"physical_block\":%" PRIu64 ",\"status\":%d}\n",
            id,off,n,u->physical_block,(int)u->status);
    return ferror(e->manifest)?INFS_STATUS_IO_ERROR:INFS_STATUS_OK;
}
int main(int argc,char **argv)
{
    if(argc!=3) {
        fprintf(stderr,"Usage: %s READ_ONLY_020_IMAGE NEW_OUTPUT_DIRECTORY\n",argv[0]);
        return 2;
    }
    struct source source={.fd=open(argv[1],O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK)};
    if(source.fd<0 || fstat(source.fd,&source.initial) ||
       !S_ISREG(source.initial.st_mode) || source.initial.st_size<0) {
        if(source.fd>=0) close(source.fd);
        fputs("Source must be an ordinary immutable experimental image.\n",stderr);
        return 1;
    }
    struct flock lock={.l_type=F_RDLCK,.l_whence=SEEK_SET};
    if(fcntl(source.fd,F_SETLK,&lock)<0) { close(source.fd); return 1; }
    struct infs20_raw_source raw={&source,(uint64_t)source.initial.st_size,read_at};
    struct infs20_store_region region; struct infs20_paged_layout layout;
    struct infs20_paged_state state;
    infs_status st=infs20_paged_discover(&raw,&region,&layout,&state);
    if(st || !unchanged(&source)) {
        fprintf(stderr,"No verified experimental paged region (status %d).\n",(int)st);
        close(source.fd); return 1;
    }
    /* Require fresh output. Never overwrite an existing file or directory. */
    if(mkdir(argv[2],0700)) { perror("output directory"); close(source.fd); return 1; }
    struct export *e=calloc(1,sizeof(*e));
    if(!e) { close(source.fd); return 1; }
    e->dir=open(argv[2],O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    int fd=e->dir<0?-1:openat(e->dir,"manifest.jsonl",
        O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    e->manifest=fd<0?NULL:fdopen(fd,"w");
    int result=1;
    if(!e->manifest) { if(fd>=0) close(fd); goto done; }
    char digest[65]; hex(state.commit.commit_digest,32,digest);
    fprintf(e->manifest,"{\"kind\":\"source\",\"offset\":%" PRIu64 ",\"sequence\":%" PRIu64 ",\"commit\":\"%s\",\"experimental\":true}\n",
            region.start,state.commit.sequence,digest);
    struct infs20_store_io io=infs20_store_region_io(&region);
    st=infs20_paged_volume_recovery_visit(&layout,&io,mapping,unit,e);
    if(!st) st=finish_allocation(e);
    if(!st && (!unchanged(&source) || e->complete+e->partial!=e->count ||
                fsync(e->dir))) st=INFS_STATUS_IO_ERROR;
    if(!st) {
        fprintf(e->manifest,"{\"kind\":\"result\",\"status\":\"%s\",\"complete_files\":%zu,\"partial_files\":%zu,\"missing_units\":%zu}\n",
                e->bad?"partial":"complete",e->complete,e->partial,e->bad);
        if(!fflush(e->manifest) && !ferror(e->manifest) &&
           !fsync(fileno(e->manifest))) result=e->bad?3:0;
    }
    if(fclose(e->manifest)) result=1;
    if(result==1) fputs("Export incomplete; do not interpret it as a complete recovery.\n",stderr);
done:
    free(e->maps); if(e->dir>=0) close(e->dir);
    free(e); close(source.fd); return result;
}
