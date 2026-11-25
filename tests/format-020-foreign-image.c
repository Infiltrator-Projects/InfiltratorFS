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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/*
 * Real formatter evidence on two private regular files, never a device or
 * caller-specified image. This is a characterization test, not a claim that
 * every formatter leaves the experimental layout intact. The independent
 * oracle compares exact before/after block bytes; it does not infer writes
 * that left identical bytes or claim to trace physical discard requests.
 */
#define BLOCKS UINT64_C(16384) /* 64 MiB */
struct image { int fd; uint64_t bytes; };
static infs_status image_read(void *opaque,uint64_t offset,uint8_t *out,size_t n)
{
    struct image *im=opaque;
    if(offset>im->bytes || n>im->bytes-offset) return INFS_STATUS_IO_ERROR;
    while(n) {
        ssize_t got=pread(im->fd,out,n,(off_t)offset);
        if(got<0 && errno==EINTR) continue;
        if(got<=0) return INFS_STATUS_IO_ERROR;
        offset+=(uint64_t)got; out+=got; n-=(size_t)got;
    }
    return INFS_STATUS_OK;
}
static infs_status image_write(struct image *im,uint64_t offset,const uint8_t *p,size_t n)
{
    if(offset>im->bytes || n>im->bytes-offset) return INFS_STATUS_IO_ERROR;
    while(n) {
        ssize_t wrote=pwrite(im->fd,p,n,(off_t)offset);
        if(wrote<0 && errno==EINTR) continue;
        if(wrote<=0) return INFS_STATUS_IO_ERROR;
        offset+=(uint64_t)wrote; p+=wrote; n-=(size_t)wrote;
    }
    return INFS_STATUS_OK;
}
static infs_status rd(void *opaque,uint64_t block,uint8_t *p)
{
    struct image *im=opaque;
    if(block>=im->bytes/INFS20_PAGE_BYTES) return INFS_STATUS_IO_ERROR;
    return image_read(im,block*INFS20_PAGE_BYTES,p,INFS20_PAGE_BYTES);
}
static infs_status wr(void *opaque,uint64_t block,const uint8_t *p)
{
    struct image *im=opaque;
    if(block>=im->bytes/INFS20_PAGE_BYTES) return INFS_STATUS_IO_ERROR;
    return image_write(im,block*INFS20_PAGE_BYTES,p,INFS20_PAGE_BYTES);
}
static infs_status fl(void *opaque)
{
    struct image *im=opaque;
    return fsync(im->fd)==0?INFS_STATUS_OK:INFS_STATUS_IO_ERROR;
}
static int private_file(const char *path,struct image *im,struct stat *identity)
{
    im->fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_NOFOLLOW,0600);
    if(im->fd<0) return -1;
    if(fstat(im->fd,identity) || !S_ISREG(identity->st_mode) ||
       identity->st_nlink!=1 || ftruncate(im->fd,(off_t)im->bytes)) return -1;
    return 0;
}
static int same_file(const char *path,const struct image *im,const struct stat *original)
{
    struct stat st,by_path;
    return !fstat(im->fd,&st) && !lstat(path,&by_path) &&
        S_ISREG(st.st_mode) && S_ISREG(by_path.st_mode) && st.st_nlink==1 &&
        st.st_size==(off_t)im->bytes && st.st_dev==original->st_dev &&
        st.st_ino==original->st_ino && by_path.st_dev==st.st_dev &&
        by_path.st_ino==st.st_ino;
}
static int reserved(const struct infs20_store_layout *l,uint64_t block)
{
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane)
        if(block>=l->lane_start[lane] &&
           block-l->lane_start[lane]<INFS20_STORE_LANE_BLOCKS) return 1;
    return 0;
}
static int init_image(struct image *im,const struct infs20_store_layout *l)
{
    uint8_t p[INFS20_PAGE_BYTES];
    for(uint64_t b=0;b<BLOCKS;++b) {
        if(reserved(l,b)) memset(p,0,sizeof(p));
        else for(size_t i=0;i<sizeof(p);++i)
            p[i]=(uint8_t)(1+(b+i)%251); /* nonzero detection sentinel */
        if(wr(im,b,p)) return -1;
    }
    return fl(im);
}
static void tx(uint8_t id[16],unsigned n)
{
    memset(id,0,16); id[0]=(uint8_t)n; id[15]=0x67;
}
static int seed(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *p,const struct infs20_store_io *io)
{
    uint8_t id[16]; tx(id,1);
    if(p?infs20_paged_volume_create(p,io,id):infs20_volume_create(l,io,id)) return -1;
    tx(id,2);
    if(p?infs20_paged_volume_write(p,io,id,"original",(const uint8_t *)"payload-one",11):
         infs20_volume_write(l,io,id,"original",(const uint8_t *)"payload-one",11)) return -1;
    tx(id,3);
    if(p?infs20_paged_volume_clone(p,io,id,"original","clone"):
         infs20_volume_clone(l,io,id,"original","clone")) return -1;
    tx(id,4);
    if(p?infs20_paged_volume_format(p,io,id,7):infs20_volume_format(l,io,id,7)) return -1;
    tx(id,5);
    if(p?infs20_paged_volume_write(p,io,id,"later",(const uint8_t *)"payload-two",11):
         infs20_volume_write(l,io,id,"later",(const uint8_t *)"payload-two",11)) return -1;
    tx(id,6);
    if(p?infs20_paged_volume_unformat(p,io,id,7):infs20_volume_unformat(l,io,id,7)) return -1;
    return 0;
}
/* Intact-source manifest, never supplied to the recovery engine. */
struct manifest {
    uint8_t ids[2][16];
    unsigned seen[2],units[2],maps;
    uint64_t block[2];
};
static infs_status map_manifest(void *opaque,const struct infs20_recovery_mapping *m)
{
    struct manifest *v=opaque;
    unsigned i;
    if((!strcmp(m->name,"original") || !strcmp(m->name,"clone")) && !m->pin) i=0;
    else if(!strcmp(m->name,"later") && m->pin==7) i=1;
    else return INFS_STATUS_CORRUPT;
    if(m->size!=11 || (v->seen[i] && memcmp(v->ids[i],m->allocation_id,16)))
        return INFS_STATUS_CORRUPT;
    memcpy(v->ids[i],m->allocation_id,16); ++v->seen[i]; ++v->maps;
    return INFS_STATUS_OK;
}
static infs_status unit_manifest(void *opaque,const struct infs20_recovery_unit *u)
{
    struct manifest *v=opaque;
    unsigned i=memcmp(v->ids[0],u->allocation_id,16)!=0;
    if(!v->seen[i] || memcmp(v->ids[i],u->allocation_id,16) || v->units[i] ||
       u->status || !u->data || u->logical_offset || u->logical_size!=11 ||
       u->physical_block>=BLOCKS ||
       memcmp(u->data,i?"payload-two":"payload-one",11))
        return INFS_STATUS_CORRUPT;
    v->block[i]=u->physical_block; ++v->units[i];
    return INFS_STATUS_OK;
}
struct proof_manifest { const uint8_t *changed; unsigned nodes,missing; };
static infs_status compare_ref(void *opaque,const struct infs20_tree_ref *r)
{
    struct proof_manifest *m=opaque;
    unsigned copies=0;
    for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
        if(r->blocks[lane]>=BLOCKS) return INFS_STATUS_CORRUPT;
        copies+=!m->changed[r->blocks[lane]];
    }
    printf("paged_proof_node=%u identical_copies=%u\n",m->nodes,copies);
    ++m->nodes;
    if(!copies) ++m->missing;
    return INFS_STATUS_OK;
}
static int formatter(const char *exe,const char *mode,const char *path)
{
    const char *args[12]={exe,NULL};
    unsigned n=1;
    if(!strcmp(mode,"ext4")) {
        args[n++]="-t"; args[n++]="ext4"; args[n++]="-F";
        args[n++]="-E"; args[n++]="nodiscard,lazy_itable_init=1,lazy_journal_init=1";
    } else if(!strcmp(mode,"fat32")) {
        args[n++]="-F"; args[n++]="32";
    } else if(!strcmp(mode,"exfat")) {
        args[n++]="-L"; args[n++]="INFS20TEST";
    } else if(!strcmp(mode,"ntfs")) {
        args[n++]="-F"; args[n++]="-Q"; args[n++]="-L"; args[n++]="INFS20TEST";
    } else return -1;
    args[n++]=path; args[n]=NULL;
    printf("formatter_command:"); for(unsigned i=0;i<n;++i) printf(" %s",args[i]);
    putchar('\n'); fflush(stdout);
    pid_t child=fork();
    if(child<0) return -1;
    if(child==0) {
        /* Only the private generated regular-file path is passed. No shell,
         * sudo, loop attachment, device path or arbitrary target. */
        char *argv[12];
        for(unsigned i=0;i<=n;++i) argv[i]=(char *)args[i];
        execv(exe,argv);
        _exit(127);
    }
    int status;
    while(waitpid(child,&status,0)<0) if(errno!=EINTR) return -1;
    return WIFEXITED(status) && WEXITSTATUS(status)==0?0:-1;
}
static int verify_file(const struct infs20_store_layout *l,
    const struct infs20_paged_layout *p,const struct infs20_store_io *io,
    uint64_t pin,const char *name,
    const char *expected,int unchanged)
{
    uint8_t b[32]; memset(b,0x5a,sizeof(b)); size_t n=99;
    infs_status st=p?infs20_paged_volume_read(p,io,pin,name,b,sizeof(b),&n):
                     infs20_volume_read(l,io,pin,name,b,sizeof(b),&n);
    printf("file=%s pin=%" PRIu64 " unchanged_payload=%d status=%d\n",
           name,pin,unchanged,(int)st);
    if(unchanged) return st || n!=strlen(expected) || memcmp(b,expected,n);
    return st!=INFS_STATUS_CORRUPT || n!=99 || b[0]!=0x5a;
}
#define REQUIRE(x) do { if(!(x)) { fprintf(stderr,"format20-foreign-image:%d: %s\n",__LINE__,#x); goto done; } } while(0)
int main(int argc,char **argv)
{
    if((argc!=3 && argc!=4) || argv[1][0]!='/' ||
       (argc==4 && strcmp(argv[3],"paged")) ||
       (strcmp(argv[2],"ext4") && strcmp(argv[2],"fat32") &&
        strcmp(argv[2],"exfat") && strcmp(argv[2],"ntfs"))) {
        fprintf(stderr,"usage: %s /absolute/formatter {ext4|fat32|exfat|ntfs} [paged]\n",argv[0]); return 2;
    }
    char dir[]="/tmp/infs20-foreign-XXXXXX";
    if(!mkdtemp(dir)) return 1;
    char before[128],after[128];
    snprintf(before,sizeof(before),"%s/before.img",dir);
    snprintf(after,sizeof(after),"%s/after.img",dir);
    struct image a={-1,BLOCKS*INFS20_PAGE_BYTES},b={-1,BLOCKS*INFS20_PAGE_BYTES};
    struct stat aid,bid;
    uint8_t *changed=NULL;
    int result=1;
    struct infs20_store_layout l={{1},{2},{3},BLOCKS,
        {0,BLOCKS/2,BLOCKS-INFS20_STORE_LANE_BLOCKS}};
    struct infs20_paged_layout paged={l,{20,BLOCKS/2+20,BLOCKS-276},
                                      {256,256,256}};
    const struct infs20_paged_layout *p=argc==4?&paged:NULL;
    struct infs20_store_io aio={&a,rd,wr,fl};
    struct infs20_store_state original;
    struct infs20_paged_state original_paged;
    struct manifest manifest={0};
    REQUIRE(!private_file(before,&a,&aid) && !private_file(after,&b,&bid));
    REQUIRE(!init_image(&a,&l) && !seed(&l,p,&aio));
    REQUIRE(!infs20_store_recover(&l,&aio,&original) && original.sequence==6);
    if(p) REQUIRE(!infs20_paged_open(p,&aio,&original_paged));
    REQUIRE(!verify_file(&l,p,&aio,0,"original","payload-one",1));
    REQUIRE(!verify_file(&l,p,&aio,7,"later","payload-two",1));
    REQUIRE(!(p?infs20_paged_volume_recovery_visit(p,&aio,map_manifest,unit_manifest,&manifest):
                infs20_volume_recovery_visit(&l,&aio,map_manifest,unit_manifest,&manifest)));
    REQUIRE(manifest.maps==3 && manifest.seen[0]==2 && manifest.seen[1]==1 &&
            manifest.units[0]==1 && manifest.units[1]==1 &&
            manifest.block[0]!=manifest.block[1]);
    uint8_t old[INFS20_PAGE_BYTES],now[INFS20_PAGE_BYTES];
    for(uint64_t i=0;i<BLOCKS;++i) REQUIRE(!rd(&a,i,old) && !wr(&b,i,old));
    REQUIRE(!fl(&b) && same_file(after,&b,&bid));
    REQUIRE(!formatter(argv[1],argv[2],after));
    REQUIRE(same_file(after,&b,&bid) && same_file(before,&a,&aid));
    changed=calloc((size_t)BLOCKS,1); REQUIRE(changed);
    uint64_t changes=0;
    for(uint64_t i=0;i<BLOCKS;++i) {
        REQUIRE(!rd(&a,i,old) && !rd(&b,i,now));
        changed[i]=(uint8_t)(memcmp(old,now,sizeof(old))!=0);
        changes+=changed[i];
    }
    REQUIRE(changes>0);
    printf("profile=%s store=%s image_bytes=%" PRIu64 " changed_4KiB_blocks=%" PRIu64 "\n",
           argv[2],p?"paged":"bounded",a.bytes,changes);
    for(uint64_t i=0;i<BLOCKS;) {
        if(!changed[i]) { ++i; continue; }
        uint64_t start=i; while(i<BLOCKS && changed[i]) ++i;
        printf("changed_range_blocks=[%" PRIu64 ",%" PRIu64 ")\n",start,i);
    }
    int proof=1;
    for(unsigned slot=0;slot<(unsigned)original.leaves+2;++slot) {
        unsigned copies=0;
        for(unsigned lane=0;lane<INFS20_LANE_COUNT;++lane) {
            uint64_t addr=l.lane_start[lane]+
                (original.sequence&1u)*INFS20_STORE_BANK_BLOCKS+slot;
            copies+=!changed[addr];
        }
        printf("latest_proof_slot=%u identical_copies=%u\n",slot,copies);
        if(!copies) proof=0;
    }
    if(p) {
        struct infs20_tree_io tree={0};
        tree.context=&a; tree.read=rd; tree.sequence=original.sequence;
        tree.member_blocks=l.member_blocks;
        memcpy(tree.set_id,l.set_id,16); memcpy(tree.scope_id,l.scope_id,16);
        memcpy(tree.member_id,l.member_id,16);
        struct proof_manifest pm={changed,0,0};
        REQUIRE(!infs20_tree_visit_refs(&tree,&original_paged.root,compare_ref,&pm));
        REQUIRE(pm.nodes>0);
        if(pm.missing) proof=0;
    }
    /* Re-open the foreign-formatted image read-only and discover geometry
     * from its bytes. The independently compared pre-format image is never
     * provided to the recovery engine. */
    REQUIRE(!close(b.fd)); b.fd=-1;
    b.fd=open(after,O_RDONLY|O_NOFOLLOW); REQUIRE(b.fd>=0 && same_file(after,&b,&bid));
    struct infs20_raw_source raw={&b,b.bytes,image_read};
    struct infs20_store_discovery found;
    struct infs20_store_region region;
    struct infs20_paged_layout found_paged;
    struct infs20_paged_state state;
    struct infs20_store_state selected;
    infs_status st;
    if(p) {
        st=infs20_paged_discover(&raw,&region,&found_paged,&state);
        if(!st) selected=state.commit;
    } else {
        st=infs20_store_discover(&raw,&found);
        if(!st) { region=found.region; selected=found.state; }
    }
    printf("discovery_status=%d latest_proof_survives=%d\n",(int)st,proof);
    if(proof) REQUIRE(!st && selected.sequence==original.sequence);
    if(!st) {
        REQUIRE(region.start==0 && selected.sequence<=original.sequence);
        printf("selected_sequence=%" PRIu64 "\n",selected.sequence);
        if(selected.sequence==original.sequence) {
            REQUIRE(proof);
            struct infs20_store_io rio=infs20_store_region_io(&region);
            const struct infs20_paged_layout *recovered=p?&found_paged:NULL;
            REQUIRE(!rio.write && !rio.flush);
            REQUIRE(!verify_file(&region.layout,recovered,&rio,0,"original","payload-one",!changed[manifest.block[0]]));
            REQUIRE(!verify_file(&region.layout,recovered,&rio,0,"clone","payload-one",!changed[manifest.block[0]]));
            REQUIRE(!verify_file(&region.layout,recovered,&rio,7,"later","payload-two",!changed[manifest.block[1]]));
        } else printf("historical_commit_only: not claimed as latest\n");
    } else REQUIRE(st==INFS_STATUS_CORRUPT || st==INFS_STATUS_NOT_FOUND);
    puts("format20-foreign-image: PASS (characterization and truthful recovery; not universal recovery)");
    result=0;
done:
    free(changed);
    if(a.fd>=0) close(a.fd);
    if(b.fd>=0) close(b.fd);
    unlink(before); unlink(after); rmdir(dir);
    return result;
}
