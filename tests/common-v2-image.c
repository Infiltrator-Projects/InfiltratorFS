// SPDX-License-Identifier: GPL-3.0-or-later
#include <infiltratr/filesystem_module_v2.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
struct disk { unsigned char *data; size_t size; };
static int disk_read(void *opaque,uint64_t offset,void *out,uint32_t length) {
    struct disk *d=opaque;
    if(offset>d->size || length>d->size-offset || offset%512 || length%512) return -1;
    memcpy(out,d->data+offset,length); return 0;
}
static void *allocate(void *context,size_t bytes) { (void)context; return malloc(bytes); }
static void release(void *context,void *p) { (void)context; free(p); }
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while (0)
int main(int argc,char **argv) {
    CHECK(argc==3);
    FILE *f=fopen(argv[2],"rb"); CHECK(f);
    CHECK(fseek(f,0,SEEK_END)==0); long size=ftell(f); CHECK(size>0); rewind(f);
    struct disk disk={(unsigned char *)malloc((size_t)size),(size_t)size}; CHECK(disk.data);
    CHECK(fread(disk.data,1,disk.size,f)==disk.size); fclose(f);
    void *d=dlopen(argv[1],RTLD_NOW|RTLD_LOCAL); if (!d) { fprintf(stderr,"%s\n",dlerror()); return 1; }
    IfsFsModuleApiEntryV2 entry; *(void **)(&entry)=dlsym(d,IFS_FS_COMMON_EXPORT_V2); CHECK(entry);
    const IfsFsModuleApiV2 *api=entry(); CHECK(ifs_fs_api_v2_valid(api,"infiltratorfs"));
    const IfsFsCoreOpsV2 *core=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_CORE);
    IfsFsHostRuntimeV2 host={.abi_version=IFS_FS_HOST_ABI_V2,.structure_bytes=sizeof(host),
      .capabilities=IFS_FS_HOST_READ_ONLY,.volume_bytes=disk.size,.logical_sector_bytes=512,
      .physical_sector_bytes=512,.maximum_transfer_bytes=4096,.context=&disk,.read_at=disk_read,.allocate=allocate,.release=release};
    uint32_t variant; CHECK(core->probe(&host,&variant)==0);
    void *v=NULL; IfsFsVolumeInfoV2 info={.structure_bytes=sizeof(info)};
    CHECK(core->mount(&host,&v,&info)==0 && v);
    char name[512]; IfsFsEntryV2 file={.structure_bytes=sizeof(file),.name=name,.name_capacity=sizeof(name)};
    CHECK(core->stat(v,"/",&file)==0 && file.type==IFS_FS_ENTRY_DIRECTORY);
    const IfsFsVolumeOpsV2 *ops=(const void *)ifs_fs_group_v2(api,IFS_FS_GROUP_VOLUME);
    CHECK(ops && ops->label && ops->query);
    uint32_t label_bytes=0; char label[128];
    CHECK(ops->label(v,label,sizeof(label),&label_bytes)==0 && strcmp(label,"Fixture19")==0);
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
    CHECK(core->unmount(v)==0);
    disk.data[0]^=1; CHECK(core->probe(&host,&variant)==IFS_FS_OK);
    CHECK(core->mount(&host,&v,&info)==IFS_FS_OK && v);
    CHECK(core->stat(v,"/written",&file)==IFS_FS_OK);
    CHECK(core->unmount(v)==0);
    memset(disk.data,0,disk.size);
    CHECK(core->probe(&host,&variant)==IFS_FS_NOT_FOUND);
    dlclose(d); free(disk.data); puts("Common V2 populated and recovery image: PASS"); return 0;
}
