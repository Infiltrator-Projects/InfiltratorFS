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
    uint64_t size;
    size_t count;
    int incomplete;
    char name[INFS20_VOLUME_NAME_MAX+1];
    struct infs20_recovery_extent ext[INFS20_VOLUME_EXTENTS];
};
struct allocation {
    uint8_t id[16],data[INFS20_VOLUME_BYTES];
    size_t size,units;
    uint32_t good;
    int partial_needed;
};
struct export {
    int dir;
    FILE *manifest;
    struct mapping *maps;
    size_t count,capacity,complete,partial,bad,allocation_count;
    struct allocation allocations[INFS20_MODEL_NODES];
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
static struct allocation *find_allocation(struct export *e,const uint8_t id[16])
{
    for(size_t i=0;i<e->allocation_count;++i)
        if(!memcmp(e->allocations[i].id,id,16)) return &e->allocations[i];
    return NULL;
}
static infs_status mapping(void *opaque,const struct infs20_recovery_mapping *m)
{
    struct export *e=opaque;
    size_t len=strlen(m->name);
    if(!len || len>INFS20_VOLUME_NAME_MAX || strchr(m->name,'/') ||
       strchr(m->name,'\\') || !strcmp(m->name,".") || !strcmp(m->name,"..") ||
       m->size>INFS20_LOGICAL_MAX || m->extent_count>INFS20_VOLUME_EXTENTS ||
       (m->extent_count && !m->extents)) return INFS_STATUS_CORRUPT;
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
    memset(item,0,sizeof(*item)); memcpy(item->allocation,m->allocation_id,16);
    item->pin=m->pin; item->size=m->size; item->count=m->extent_count;
    memcpy(item->name,m->name,len+1);
    char root[33],allocation[33]; hex(m->root_id,16,root); hex(m->allocation_id,16,allocation);
    fprintf(e->manifest,"{\"kind\":\"mapping\",\"root\":\"%s\",\"allocation\":\"%s\",\"pin\":%" PRIu64 ",\"role\":%u,\"size\":%" PRIu64 ",\"extent_count\":%zu,\"name\":",
            root,allocation,m->pin,(unsigned)m->role,m->size,m->extent_count);
    json_string(e->manifest,m->name); fputs("}\n",e->manifest);
    uint64_t previous=0;
    for(size_t i=0;i<m->extent_count;++i) {
        const struct infs20_recovery_extent *x=&m->extents[i];
        if(!x->length || x->logical_offset<previous || x->logical_offset>m->size ||
           x->length>m->size-x->logical_offset || !x->allocation_size ||
           x->allocation_size>INFS20_VOLUME_BYTES ||
           x->allocation_offset>x->allocation_size ||
           x->length>x->allocation_size-x->allocation_offset)
            return INFS_STATUS_CORRUPT;
        previous=x->logical_offset+x->length; item->ext[i]=*x;
        struct allocation *a=find_allocation(e,x->allocation_id);
        if(!a) {
            if(e->allocation_count==INFS20_MODEL_NODES) return INFS_STATUS_NO_SPACE;
            a=&e->allocations[e->allocation_count++];
            memcpy(a->id,x->allocation_id,16); a->size=x->allocation_size;
        } else if(a->size!=x->allocation_size) return INFS_STATUS_CORRUPT;
        hex(x->allocation_id,16,allocation);
        fprintf(e->manifest,"{\"kind\":\"extent\",\"pin\":%" PRIu64 ",\"allocation\":\"%s\",\"file_offset\":%" PRIu64 ",\"allocation_offset\":%zu,\"size\":%zu,\"name\":",
                m->pin,allocation,x->logical_offset,x->allocation_offset,x->length);
        json_string(e->manifest,m->name); fputs("}\n",e->manifest);
    }
    return ferror(e->manifest)?INFS_STATUS_IO_ERROR:INFS_STATUS_OK;
}
static infs_status unit(void *opaque,const struct infs20_recovery_unit *u)
{
    struct export *e=opaque;
    struct allocation *a=find_allocation(e,u->allocation_id);
    if(!a) return INFS_STATUS_CORRUPT;
    size_t off=a->units*INFS20_PAGE_BYTES;
    if(off>=a->size || a->units>=INFS20_VOLUME_UNITS) return INFS_STATUS_CORRUPT;
    size_t n=a->size-off; if(n>INFS20_PAGE_BYTES) n=INFS20_PAGE_BYTES;
    if(u->logical_offset!=off || u->logical_size!=n || (u->status==0)!=!!u->data)
        return INFS_STATUS_CORRUPT;
    if(!u->status) {
        memcpy(a->data+off,u->data,n); a->good|=UINT32_C(1)<<a->units;
    } else ++e->bad;
    ++a->units;
    char id[33]; hex(u->allocation_id,16,id);
    fprintf(e->manifest,"{\"kind\":\"unit\",\"allocation\":\"%s\",\"offset\":%zu,\"size\":%zu,\"physical_block\":%" PRIu64 ",\"status\":%d}\n",
            id,off,n,u->physical_block,(int)u->status);
    return ferror(e->manifest)?INFS_STATUS_IO_ERROR:INFS_STATUS_OK;
}
/* Complete export preserves genuine sparse gaps, never substitutes holes
 * for corrupt/missing mapped units. Verify all slices before creation. */
static int slice_good(const struct allocation *a,const struct infs20_recovery_extent *x)
{
    size_t first=x->allocation_offset/INFS20_PAGE_BYTES;
    size_t last=(x->allocation_offset+x->length-1)/INFS20_PAGE_BYTES;
    for(size_t i=first;i<=last;++i) if(!(a->good&(UINT32_C(1)<<i))) return 0;
    return 1;
}
static int write_sparse(struct export *e,int dir,const struct mapping *m)
{
    int fd=openat(dir,m->name,O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) return -1;
    int failed=ftruncate(fd,(off_t)m->size)!=0;
    for(size_t i=0;i<m->count && !failed;++i) {
        const struct infs20_recovery_extent *x=&m->ext[i];
        struct allocation *a=find_allocation(e,x->allocation_id);
        if(!a || !slice_good(a,x)) { failed=1; break; }
        size_t done=0;
        while(done<x->length) {
            ssize_t n=pwrite(fd,a->data+x->allocation_offset+done,
                             x->length-done,(off_t)(x->logical_offset+done));
            if(n<0 && errno==EINTR) continue;
            if(n<=0) { failed=1; break; }
            done+=(size_t)n;
        }
    }
    if(!failed && fsync(fd)) failed=1;
    if(close(fd)) failed=1;
    if(failed) { (void)unlinkat(dir,m->name,0); return -1; }
    return fsync(dir);
}
static infs_status finish_export(struct export *e)
{
    for(size_t i=0;i<e->allocation_count;++i) {
        struct allocation *a=&e->allocations[i];
        if(a->units!=1+(a->size-1)/INFS20_PAGE_BYTES) return INFS_STATUS_CORRUPT;
    }
    for(size_t i=0;i<e->count;++i) {
        struct mapping *m=&e->maps[i]; int complete=1;
        for(size_t j=0;j<m->count;++j) {
            struct allocation *a=find_allocation(e,m->ext[j].allocation_id);
            if(!a) return INFS_STATUS_CORRUPT;
            if(!slice_good(a,&m->ext[j])) complete=0;
        }
        if(!complete) {
            ++e->partial; m->incomplete=1;
            for(size_t j=0;j<m->count;++j)
                find_allocation(e,m->ext[j].allocation_id)->partial_needed=1;
            continue;
        }
        char dir[48],path[128],id[33]; hex(m->allocation,16,id);
        if(m->pin) snprintf(dir,sizeof(dir),"pin-%" PRIu64,m->pin);
        else strcpy(dir,"active");
        int fd=subdir(e->dir,dir); if(fd<0) return INFS_STATUS_IO_ERROR;
        int result=write_sparse(e,fd,m); if(close(fd)) result=-1;
        if(result) return INFS_STATUS_IO_ERROR;
        snprintf(path,sizeof(path),"%s/%s",dir,m->name);
        fprintf(e->manifest,"{\"kind\":\"file\",\"allocation\":\"%s\",\"path\":",id);
        json_string(e->manifest,path); fputs("}\n",e->manifest); ++e->complete;
    }
    for(size_t i=0;i<e->allocation_count;++i) {
        struct allocation *a=&e->allocations[i];
        if(!a->partial_needed) continue;
        char id[33]; hex(a->id,16,id);
        int fd=subdir(e->dir,"partial"); if(fd<0) return INFS_STATUS_IO_ERROR;
        int result=0;
        for(size_t j=0;j<a->units;++j) {
            if(!(a->good&(UINT32_C(1)<<j))) continue;
            size_t off=j*INFS20_PAGE_BYTES,n=a->size-off;
            if(n>INFS20_PAGE_BYTES) n=INFS20_PAGE_BYTES;
            /* A valid allocation unit can contain unreferenced bytes after
             * truncation, punching or shared slicing. Export only the union
             * of mapped ranges of incomplete files, never the entire old
             * allocation merely because its checksum still verifies. */
            uint8_t covered[INFS20_PAGE_BYTES]={0};
            for(size_t k=0;k<e->count;++k) {
                const struct mapping *m=&e->maps[k];
                if(!m->incomplete) continue;
                for(size_t q=0;q<m->count;++q) {
                    const struct infs20_recovery_extent *x=&m->ext[q];
                    if(memcmp(x->allocation_id,a->id,16)) continue;
                    size_t from=x->allocation_offset>off?x->allocation_offset:off;
                    size_t end=x->allocation_offset+x->length;
                    if(end>off+n) end=off+n;
                    if(from<end) memset(covered+from-off,1,end-from);
                }
            }
            for(size_t k=0;k<n;) {
                if(!covered[k]) { ++k; continue; }
                size_t start=k;
                while(k<n && covered[k]) ++k;
                size_t pos=off+start,length=k-start;
                char name[80]; snprintf(name,sizeof(name),"%s-%zu.bin",id,pos);
                if(file_write(fd,name,a->data+pos,length)) { result=-1; break; }
                fprintf(e->manifest,"{\"kind\":\"fragment\",\"allocation\":\"%s\",\"offset\":%zu,\"size\":%zu,\"path\":\"partial/%s\"}\n",
                        id,pos,length,name);
            }
            if(result) break;
        }
        if(close(fd)) result=-1;
        if(result) return INFS_STATUS_IO_ERROR;
    }
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
    if(!st) st=finish_export(e);
    if(!st && (!unchanged(&source) || e->complete+e->partial!=e->count ||
                fsync(e->dir))) st=INFS_STATUS_IO_ERROR;
    if(!st) {
        fprintf(e->manifest,"{\"kind\":\"result\",\"status\":\"%s\",\"complete_files\":%zu,\"partial_files\":%zu,\"missing_units\":%zu}\n",
                e->partial?"partial":"complete",e->complete,e->partial,e->bad);
        if(!fflush(e->manifest) && !ferror(e->manifest) &&
           !fsync(fileno(e->manifest))) result=e->partial?3:0;
    }
    if(fclose(e->manifest)) result=1;
    if(result==1) fputs("Export incomplete; do not interpret it as a complete recovery.\n",stderr);
done:
    free(e->maps); if(e->dir>=0) close(e->dir);
    free(e); close(source.fd); return result;
}
