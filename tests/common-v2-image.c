// SPDX-License-Identifier: GPL-3.0-or-later
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct disk {
    unsigned char *data;
    size_t size;
    unsigned sector, writes, flushes, random, fail_after, fail_flush_after;
    int fail_write, fail_flush, fail_allocate, fail_read;
};
static const IfsFsCoreOpsV2 *unmount_during_read;
static void *active_volume;
static int observed_unmount;
static int disk_read(void *opaque,uint64_t offset,void *out,uint32_t length) {
    if (unmount_during_read) {
        const IfsFsCoreOpsV2 *ops=unmount_during_read;
        unmount_during_read=NULL;
        observed_unmount=ops->unmount(active_volume);
        if (observed_unmount!=IFS_FS_BUSY) return -1;
    }
    struct disk *d=opaque;
    if(d->fail_read || offset>d->size || length>d->size-offset || offset%d->sector || length%d->sector) return -1;
    memcpy(out,d->data+offset,length); return 0;
}
static void *allocate(void *context,size_t bytes) {
    struct disk *d=context;
    return d->fail_allocate ? NULL : malloc(bytes);
}
static void release(void *context,void *p) { (void)context; free(p); }
static int disk_write(void *opaque,uint64_t offset,const void *in,uint32_t length) {
    struct disk *d=opaque;
    if(offset>d->size || length>d->size-offset || offset%d->sector || length%d->sector ||
       (d->fail_write && d->writes>=d->fail_after)) return -1;
    memcpy(d->data+offset,in,length); ++d->writes; return 0;
}
static int disk_flush(void *opaque) {
    struct disk *d=opaque;
    ++d->flushes;
    return d->fail_flush && d->flushes>=d->fail_flush_after ? -1 : 0;
}
static uint64_t clock_ns(void *opaque) { (void)opaque; return UINT64_C(1234567000000000); }
static int entropy(void *opaque,void *out,uint32_t length) {
    struct disk *d=opaque; memset(out,(int)++d->random,length); return 0;
}
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc,char **argv) {
    CHECK(argc==5);
    FILE *f=fopen(argv[2],"rb"); CHECK(f);
    CHECK(fseek(f,0,SEEK_END)==0); long size=ftell(f); CHECK(size>0); rewind(f);
    struct disk disk={.data=(unsigned char *)malloc((size_t)size),.size=(size_t)size,
                      .sector=(unsigned)strtoul(argv[3],NULL,10)};
    CHECK(disk.data && (disk.sector==512 || disk.sector==4096));
    CHECK(fread(disk.data,1,disk.size,f)==disk.size); fclose(f);
    void *d=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if (!d) { fprintf(stderr,"%s\n",dlerror()); return 1; }
    IfsFsModuleApiEntryV2 entry; *(void **)(&entry)=dlsym(d,IFS_FS_COMMON_EXPORT_V2); CHECK(entry);
    const IfsFsModuleApiV2 *api=entry(); CHECK(ifs_fs_api_v2_valid(api,"infiltratorfs"));
    CHECK((api->runtime_profiles & (uint32_t)strtoul(argv[4],NULL,10))!=0);
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    IfsFsHostRuntimeV2 host={.abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(host),
      .capabilities=IFS_FS_HOST_READ_ONLY,.volume_bytes=disk.size,.logical_sector_bytes=disk.sector,
      .physical_sector_bytes=disk.sector,.maximum_transfer_bytes=4096,.context=&disk,.read_at=disk_read,.allocate=allocate,.release=release};
    uint32_t variant; CHECK(core->probe(&host,&variant)==0);
    void *v=NULL; IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    CHECK(core->mount(&host,&v,&info)==0 && v);
    char name[512]; IfsFsEntryV2 file={.structure_bytes=sizeof(file),.name=name,.name_capacity=sizeof(name)};
    CHECK(core->stat(v,"/",&file)==0 && file.type==IFS_FS_ENTRY_DIRECTORY);
    char tiny[2]={'X','X'};
    IfsFsEntryV2 narrow={.structure_bytes=sizeof(narrow),.name=tiny,.name_capacity=sizeof(tiny)};
    CHECK(core->stat(v,"/written",&narrow)==IFS_FS_NO_SPACE);
    CHECK(narrow.name_bytes==0u && tiny[0]=='X' && tiny[1]=='X');
    const IfsFsVolumeOpsV2 *ops=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(ops && ops->label && ops->query);
    const int hosted=(api->runtime_profiles&IFS_FS_PROFILE_HOSTED)!=0;
    CHECK(!!ops->scrub==hosted && !!ops->check==hosted && !ops->repair);
    if (hosted) {
        uint64_t errors=UINT64_MAX;
        unsigned writes=disk.writes;
        CHECK(ops->check(v,0,&errors)==IFS_FS_OK && !errors);
        CHECK(ops->check(v,1,&errors)==IFS_FS_UNSUPPORTED && !errors);
        CHECK(ops->scrub(v,0,&errors)==IFS_FS_OK && !errors);
        CHECK(ops->scrub(v,1,&errors)==IFS_FS_UNSUPPORTED && !errors);
        CHECK(disk.writes==writes);
    }
    const IfsFsMetadataOpsV2 *meta=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_METADATA);
    CHECK(meta && meta->get && meta->set &&
          meta->symlink && meta->readlink && meta->hardlink &&
          meta->header.capabilities==(IFS_FS_META_GET_MODE|IFS_FS_META_GET_TIMES|IFS_FS_META_SET_MODE|
              IFS_FS_META_SYMLINK|IFS_FS_META_READLINK|IFS_FS_META_HARDLINK));
    IfsFsMetadataV2 metadata={.structure_bytes=sizeof(metadata)};
    CHECK(meta->get(v,"/written",&metadata)==IFS_FS_OK);
    CHECK((metadata.mask&IFS_FS_META_GET_MODE) && (metadata.mode&0170000u)==0100000u);
    metadata.mask=IFS_FS_META_SET_MODE;
    CHECK(meta->set(v,"/written",&metadata)==IFS_FS_ACCESS);
    CHECK(meta->symlink(v,"/denied","written")==IFS_FS_ACCESS);
    CHECK(meta->hardlink(v,"/written","/denied")==IFS_FS_ACCESS);
    uint32_t label_bytes=0; char label[128];
    CHECK(ops->label(v,label,sizeof(label),&label_bytes)==0 && strcmp(label,"Fixture19")==0);
    CHECK(ops->label(v,tiny,sizeof(tiny),&label_bytes)==IFS_FS_NO_SPACE);
    CHECK(label_bytes==0u && tiny[0]=='X' && tiny[1]=='X');
    CHECK(ops->query(v,&info)==0 && info.total_bytes==disk.size);
    int saw_written=0;
    for (unsigned i=0;;i++) {
        int status=core->directory_entry(v,"/",i,&file); if(status==IFS_FS_END) break; CHECK(status==0);
        printf("entry=%s type=%u size=%llu\n",name,file.type,(unsigned long long)file.size_bytes);
        if(file.type==IFS_FS_ENTRY_FILE && file.size_bytes) {
            char path[520]; snprintf(path,sizeof(path),"/%s",name);
            unsigned char *data=malloc((size_t)file.size_bytes); CHECK(data);
            CHECK(core->read(v,path,0,data,(uint32_t)file.size_bytes)==0);
            if (strcmp(path,"/written")==0) {
                const char expected[]="Format 0.19 committed payload";
                CHECK(file.size_bytes==sizeof(expected)-1 && memcmp(data,expected,sizeof(expected)-1)==0);
                saw_written=1;
            }
            free(data);
        }
    }
    CHECK(saw_written);
    CHECK(core->stat(v,"/not-here",&file)==IFS_FS_NOT_FOUND);
    CHECK(core->write(v,"/written",0,"x",1)==IFS_FS_ACCESS);
    char retained_data[30]={0};
    active_volume=v;
    unmount_during_read=core;
    observed_unmount=IFS_FS_INVALID;
    CHECK(core->read(v,"/written",0,retained_data,29)==IFS_FS_OK);
    CHECK(!unmount_during_read && observed_unmount==IFS_FS_BUSY);
    void *expired=v;
    CHECK(core->unmount(v)==0);
    CHECK(core->unmount(expired)==IFS_FS_INVALID);
    CHECK(core->stat(expired,"/",&file)==IFS_FS_INVALID);
    /* An existing V2 host can supply only the original sized prefix. */
    volatile size_t prefix_bytes=IFS_FS_HOST_V2_BASE_BYTES;
    IfsFsHostRuntimeV2 *prefix=malloc(prefix_bytes);
    CHECK(prefix);
    memcpy(prefix,&host,IFS_FS_HOST_V2_BASE_BYTES);
    prefix->structure_bytes=IFS_FS_HOST_V2_BASE_BYTES;
    CHECK(core->mount(prefix,&v,&info)==IFS_FS_OK && v && v!=expired);
    /* The caller owns descriptor storage, not the mounted session. */
    memset(prefix,0,prefix_bytes);
    free(prefix);
    CHECK(core->stat(expired,"/",&file)==IFS_FS_INVALID);
    char prefix_data[30]={0};
    CHECK(core->read(v,"/written",0,prefix_data,29)==IFS_FS_OK);
    CHECK(memcmp(prefix_data,"Format 0.19 committed payload",29)==0);
    CHECK(core->unmount(v)==IFS_FS_OK);
    /* A writable mount requires an exclusive host, durable writes, clock and
     * genuine host-provided entropy. Absence must fail before any writes. */
    host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE|IFS_FS_HOST_CLOCK;
    host.write_at=disk_write; host.flush=disk_flush;
    host.monotonic_ns=clock_ns; host.realtime_ns=clock_ns;
    CHECK(core->mount(&host,&v,&info)==IFS_FS_UNSUPPORTED && !v && !disk.writes);
    host.random_bytes=entropy;
    CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
    CHECK(core->create(v,"/created",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
    metadata.structure_bytes=sizeof(metadata);
    CHECK(meta->get(v,"/created",&metadata)==IFS_FS_OK &&
          metadata.mode==0100644u && (metadata.mask&IFS_FS_META_GET_TIMES) &&
          metadata.created_ns==(int64_t)clock_ns(NULL));
    metadata.mask=IFS_FS_META_SET_MODE;
    metadata.mode=0100600u;metadata.owner=123u;metadata.group=456u;
    CHECK(meta->set(v,"/created",&metadata)==IFS_FS_OK);
    CHECK(meta->get(v,"/created",&metadata)==IFS_FS_OK &&
          metadata.mode==0100600u && metadata.owner==123u && metadata.group==456u);
    const char payload[]="durable v2 filesystem write";
    CHECK(core->write(v,"/created",0,payload,sizeof(payload)-1)==IFS_FS_OK);
    CHECK(ops->sync(v)==IFS_FS_OK && disk.writes && disk.flushes);
    CHECK(core->rename(v,"/created","/renamed")==IFS_FS_OK);
    CHECK(meta->symlink(v,"/relative-link","renamed")==IFS_FS_OK);
    CHECK(meta->hardlink(v,"/renamed","/hard-link")==IFS_FS_OK);
    CHECK(meta->hardlink(v,"/renamed","/hard-link")==IFS_FS_EXISTS);
    CHECK(core->remove(v,"/renamed")==IFS_FS_OK);
    CHECK(core->rename(v,"/hard-link","/renamed")==IFS_FS_OK);
    CHECK(ops->sync(v)==IFS_FS_OK);
    /* Use non-inline, non-repeating file content so the exact physical
     * data block can be located independently of the provider's parser. */
    unsigned char scrub_data[8192];
    if (hosted) {
        uint32_t seed=0x4c318921u;
        for(size_t i=0;i<sizeof(scrub_data);++i) {
            seed^=seed<<13;seed^=seed>>17;seed^=seed<<5;
            scrub_data[i]=(unsigned char)seed;
        }
        CHECK(core->create(v,"/scrub-data",IFS_FS_ENTRY_FILE)==IFS_FS_OK);
        CHECK(core->write(v,"/scrub-data",0,scrub_data,sizeof(scrub_data))==IFS_FS_OK);
        CHECK(ops->sync(v)==IFS_FS_OK);
    }
    CHECK(core->unmount(v)==IFS_FS_OK);
    host.capabilities=IFS_FS_HOST_READ_ONLY; host.write_at=NULL; host.flush=NULL;
    host.monotonic_ns=NULL; host.realtime_ns=NULL; host.random_bytes=NULL;
    CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
    CHECK(meta->get(v,"/renamed",&metadata)==IFS_FS_OK &&
          metadata.mode==0100600u && metadata.owner==123u && metadata.group==456u &&
          metadata.created_ns==(int64_t)clock_ns(NULL));
    char target[32]={0};
    uint32_t target_bytes=UINT32_MAX;
    CHECK(meta->readlink(v,"/relative-link",target,sizeof(target),&target_bytes)==IFS_FS_OK &&
          target_bytes==7u && !strcmp(target,"renamed"));
    CHECK(meta->readlink(v,"/relative-link",tiny,sizeof(tiny),&target_bytes)==IFS_FS_NO_SPACE &&
          !target_bytes && tiny[0]=='X' && tiny[1]=='X');
    CHECK(meta->get(v,"/relative-link",&metadata)==IFS_FS_OK &&
          (metadata.mode&0170000u)==0120000u);
    char readback[sizeof(payload)]={0};
    CHECK(core->read(v,"/renamed",0,readback,sizeof(payload)-1)==IFS_FS_OK);
    CHECK(memcmp(readback,payload,sizeof(payload)-1)==0);
    if (hosted) {
        uint64_t errors=UINT64_MAX;
        unsigned writes=disk.writes;
        CHECK(ops->scrub(v,0,&errors)==IFS_FS_OK && !errors);
        size_t physical=0;
        for(size_t at=4096u;at+4096u<=disk.size;at+=4096u)
            if (!memcmp(disk.data+at,scrub_data,4096u)) {physical=at;break;}
        CHECK(physical);
        disk.data[physical+17u]^=1u;
        /* Structural checking is not a substitute for payload scrubbing. */
        CHECK(ops->check(v,0,&errors)==IFS_FS_OK && !errors);
        int rc=ops->scrub(v,0,&errors);
        CHECK(rc==IFS_FS_CORRUPT && errors>0u);
        disk.data[physical+17u]^=1u;
        CHECK(ops->scrub(v,0,&errors)==IFS_FS_OK && !errors);
        CHECK(disk.writes==writes);
    }
    CHECK(core->unmount(v)==IFS_FS_OK);
    host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE|IFS_FS_HOST_CLOCK;
    host.write_at=disk_write; host.flush=disk_flush;
    host.monotonic_ns=clock_ns; host.realtime_ns=clock_ns; host.random_bytes=entropy;
    CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
    disk.fail_write=1; disk.fail_after=disk.writes;
    CHECK(core->write(v,"/renamed",0,"changed",7)!=IFS_FS_OK);
    CHECK(core->unmount(v)==IFS_FS_OK);
    disk.fail_write=0;
    host.capabilities=IFS_FS_HOST_READ_ONLY; host.write_at=NULL; host.flush=NULL;
    host.monotonic_ns=NULL; host.realtime_ns=NULL; host.random_bytes=NULL;
    CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
    memset(readback,0,sizeof(readback));
    CHECK(core->read(v,"/renamed",0,readback,sizeof(payload)-1)==IFS_FS_OK);
    CHECK(memcmp(readback,payload,sizeof(payload)-1)==0);
    CHECK(core->unmount(v)==IFS_FS_OK);
    unsigned char *stable=malloc(disk.size); CHECK(stable);
    memcpy(stable,disk.data,disk.size);
    const unsigned failures[]={0,1,4,12,32};
    for (unsigned index=0;index<sizeof(failures)/sizeof(failures[0]);++index) {
        memcpy(disk.data,stable,disk.size);
        host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE|IFS_FS_HOST_CLOCK;
        host.write_at=disk_write; host.flush=disk_flush;
        host.monotonic_ns=clock_ns; host.realtime_ns=clock_ns; host.random_bytes=entropy;
        CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
        disk.fail_write=1; disk.fail_after=disk.writes+failures[index];
        (void)core->write(v,"/renamed",0,"changed",7);
        CHECK(core->unmount(v)==IFS_FS_OK);
        disk.fail_write=0;
        host.capabilities=IFS_FS_HOST_READ_ONLY; host.write_at=NULL; host.flush=NULL;
        host.monotonic_ns=NULL; host.realtime_ns=NULL; host.random_bytes=NULL;
        CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
        memset(readback,0,sizeof(readback));
        CHECK(core->read(v,"/renamed",0,readback,sizeof(payload)-1)==IFS_FS_OK);
        char changed[sizeof(payload)]; memcpy(changed,payload,sizeof(payload));
        memcpy(changed,"changed",7);
        CHECK(memcmp(readback,payload,sizeof(payload)-1)==0 ||
              memcmp(readback,changed,sizeof(payload)-1)==0);
        CHECK(core->unmount(v)==IFS_FS_OK);
    }
    const unsigned flush_failures[]={0,1,2,3,5,8};
    unsigned exercised_flush_failures=0;
    for (unsigned index=0;index<sizeof(flush_failures)/sizeof(flush_failures[0]);++index) {
        memcpy(disk.data,stable,disk.size);
        host.capabilities=IFS_FS_HOST_EXCLUSIVE|IFS_FS_HOST_DURABLE|IFS_FS_HOST_CLOCK;
        host.write_at=disk_write; host.flush=disk_flush;
        host.monotonic_ns=clock_ns; host.realtime_ns=clock_ns; host.random_bytes=entropy;
        CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
        disk.fail_flush=1; disk.fail_flush_after=disk.flushes+1u+flush_failures[index];
        int result=core->write(v,"/renamed",0,"changed",7);
        if(disk.flushes>=disk.fail_flush_after) {
            ++exercised_flush_failures;
            CHECK(result!=IFS_FS_OK);
        }
        CHECK(core->unmount(v)==IFS_FS_OK);
        disk.fail_flush=0;
        host.capabilities=IFS_FS_HOST_READ_ONLY; host.write_at=NULL; host.flush=NULL;
        host.monotonic_ns=NULL; host.realtime_ns=NULL; host.random_bytes=NULL;
        CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
        memset(readback,0,sizeof(readback));
        CHECK(core->read(v,"/renamed",0,readback,sizeof(payload)-1)==IFS_FS_OK);
        char changed[sizeof(payload)]; memcpy(changed,payload,sizeof(payload));
        memcpy(changed,"changed",7);
        CHECK(memcmp(readback,payload,sizeof(payload)-1)==0 ||
              memcmp(readback,changed,sizeof(payload)-1)==0);
        CHECK(core->unmount(v)==IFS_FS_OK);
    }
    CHECK(exercised_flush_failures>=3u);
    memcpy(disk.data,stable,disk.size); free(stable);
    disk.data[0]^=1; CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
    CHECK(core->stat(v,"/written",&file)==IFS_FS_OK);
    CHECK(core->unmount(v)==0);
    /* Host scratch-sector allocation is a resource error, not an I/O
     * failure or a negative filesystem identification. */
    disk.fail_allocate=1;
    CHECK(core->probe(&host,&variant)==IFS_FS_NO_MEMORY);
    disk.fail_allocate=0;
    /* Keep one locator signature but erase its graph and all checkpoints. */
    memset(disk.data,0,disk.size);
    memcpy(disk.data+4096,"INFSL019",8);
    CHECK(core->probe(&host,&variant)==IFS_FS_CORRUPT);
    memset(disk.data,0,disk.size);
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND);
    disk.fail_read=1;
    CHECK(core->probe(&host,&variant)==IFS_FS_IO_ERROR);
    disk.fail_read=0;
    dlclose(d); free(disk.data); puts("Common V2 populated and recovery image: PASS"); return 0;
}
