// SPDX-License-Identifier: GPL-3.0-or-later
// Mounted locking qualification. All mutators are separate processes so their
// page caches, descriptors and VFS scheduling compete as they do in practice.
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#define WORKERS 6
#define FILES 24
#define FILE_BYTES 32768
#define REGION (1024 * 1024)
#define CHUNK 65536
#define RECORD 128
#define RECORDS 256
static char *root;
static void fail(const char *where) { perror(where); exit(1); }
static void check(bool ok, const char *where) { if (!ok) { errno=EIO; fail(where); } }
static char *join(const char *a, const char *b) {
    char *s=NULL; if (asprintf(&s,"%s/%s",a,b)<0) fail("path"); return s;
}
static void write_all(int fd, const void *p, size_t len) {
    const char *data=p; while(len) { ssize_t n=write(fd,data,len); if(n<0&&errno==EINTR)continue;
        if(n<=0)fail("write");
        data+=n;len-=(size_t)n; }
}
static void read_exact(int fd, void *p, size_t len) {
    char *data=p; while(len) { ssize_t n=read(fd,data,len); if(n<0&&errno==EINTR)continue;
        if(n<=0)fail("read");
        data+=n;len-=(size_t)n; }
}
static void payload(unsigned char *buf,int w,int i) {
    int seed=(w*37+i*19+11)&255;
    for(size_t n=0;n<FILE_BYTES;n++)buf[n]=(unsigned char)(seed+n*13);
}
static void namespace_worker(const char *dir,int w) {
    unsigned char data[FILE_BYTES]; char name[64],value[32];
    for(int i=0;i<FILES;i++) {
        snprintf(name,sizeof name,"w%02d-%03d.tmp",w,i); char *tmp=join(dir,name);
        snprintf(name,sizeof name,"w%02d-%03d.dat",w,i); char *final=join(dir,name);
        char *link=NULL; if(asprintf(&link,"%s.link",final)<0)fail("link path");
        payload(data,w,i); int fd=open(tmp,O_CREAT|O_EXCL|O_RDWR,0600);
        if(fd<0)fail("create namespace");
        write_all(fd,data,sizeof data);
        if(fsync(fd)||close(fd)||rename(tmp,final)||linkat(AT_FDCWD,final,AT_FDCWD,link,0))fail("publish namespace");
        int length=snprintf(value,sizeof value,"%d:%d",w,i);
        if(setxattr(final,"user.infiltratorfs-concurrency",value,(size_t)length,0)||unlink(link))fail("namespace xattr/link");
        if(i%6==0){int dfd=open(dir,O_RDONLY|O_DIRECTORY);if(dfd<0||fsync(dfd)||close(dfd))fail("directory fsync");}
        free(tmp);free(final);free(link);
    }
}
static void verify_namespace(const char *dir) {
    DIR *d=opendir(dir);if(!d)fail("open namespace");struct dirent *ent;int count=0;
    while((ent=readdir(d))) { size_t len=strlen(ent->d_name); if(len>=4&&!strcmp(ent->d_name+len-4,".dat"))count++; }
    closedir(d);check(count==WORKERS*FILES,"namespace count");
    unsigned char expected[FILE_BYTES],found[FILE_BYTES]; char name[64],value[64];
    for(int w=0;w<WORKERS;w++)for(int i=0;i<FILES;i++){
        snprintf(name,sizeof name,"w%02d-%03d.dat",w,i);char *path=join(dir,name);
        int fd=open(path,O_RDONLY);if(fd<0)fail("open namespace file");read_exact(fd,found,sizeof found);
        unsigned char extra;check(read(fd,&extra,1)==0,"namespace length");close(fd);
        payload(expected,w,i);check(!memcmp(expected,found,sizeof found),"namespace payload");
        int n=getxattr(path,"user.infiltratorfs-concurrency",value,sizeof value);
        int length=snprintf(name,sizeof name,"%d:%d",w,i);
        check(n==length&&!memcmp(value,name,(size_t)length),"namespace xattr");free(path);
    }
}
static void pwrite_all(int fd,const void *data,size_t len,off_t at) {
    const char *p=data;while(len){ssize_t n=pwrite(fd,p,len,at);if(n<0&&errno==EINTR)continue;
        if(n<=0)fail("pwrite");
        p+=n;at+=n;len-=(size_t)n;}
}
static void shared_worker(const char *path,int w) {
    int fd=open(path,O_RDWR);if(fd<0)fail("open shared");unsigned char data[CHUNK];
    for(int c=0;c<REGION/CHUNK;c++){
        memset(data,(w*41+c*23+7)&255,sizeof data);
        pwrite_all(fd,data,sizeof data,(off_t)w*REGION+(off_t)c*CHUNK);
        if(c%4==3&&fsync(fd))fail("shared fsync");
    }if(fsync(fd)||close(fd))fail("close shared");
}
static void verify_shared(const char *path) {
    int fd=open(path,O_RDONLY);if(fd<0)fail("verify shared open");unsigned char data[CHUNK];
    for(int w=0;w<WORKERS;w++)for(int c=0;c<REGION/CHUNK;c++){
        ssize_t n=pread(fd,data,sizeof data,(off_t)w*REGION+(off_t)c*CHUNK);
        check(n==(ssize_t)sizeof data,"shared read size");
        for(size_t i=0;i<sizeof data;i++)check(data[i]==((w*41+c*23+7)&255),"shared data");
    }close(fd);
}
static void make_record(char *data,int w,int seq) {
    int n=snprintf(data,RECORD,"%02d:%04d:",w,seq);
    check(n>0&&n<RECORD,"append header");memset(data+n,'A'+w,RECORD-(size_t)n);
}
static void append_worker(const char *path,int w) {
    int fd=open(path,O_WRONLY|O_APPEND);if(fd<0)fail("append open");char data[RECORD];
    for(int seq=0;seq<RECORDS;seq++){
        make_record(data,w,seq);check(write(fd,data,sizeof data)==RECORD,"atomic append");
        if(seq%64==63&&fdatasync(fd))fail("append fdatasync");
    }if(close(fd))fail("append close");
}
static void verify_append(const char *path) {
    int fd=open(path,O_RDONLY);if(fd<0)fail("append verify open");
    struct stat st;if(fstat(fd,&st))fail("append stat");
    check(st.st_size==WORKERS*RECORDS*RECORD,"append length");
    unsigned char seen[WORKERS][RECORDS]={0};char data[RECORD],expected[RECORD];
    for(int n=0;n<WORKERS*RECORDS;n++){
        read_exact(fd,data,sizeof data);int w=-1,seq=-1;
        check(sscanf(data,"%02d:%04d:",&w,&seq)==2&&w>=0&&w<WORKERS&&seq>=0&&seq<RECORDS,"append header");
        make_record(expected,w,seq);check(!seen[w][seq]&&!memcmp(data,expected,RECORD),"append torn/duplicate");seen[w][seq]=1;
    }close(fd);
}
static void xattr_worker(const char *path,int worker) {
    if(worker<3){char name[64],value[64],observed[64];snprintf(name,sizeof name,"user.infiltratorfs-rw-%d",worker);
        for(int i=0;i<128;i++) {int n=snprintf(value,sizeof value,"writer-%d-iteration-%03d",worker,i);
            if(setxattr(path,name,value,(size_t)n,0))fail("xattr set");
            int got=getxattr(path,name,observed,sizeof observed);
            check(got==n&&!memcmp(value,observed,(size_t)n),"xattr writer readback");}
    }else{for(int i=0;i<256;i++){
        char names[512];ssize_t listed=listxattr(path,names,sizeof names);if(listed<0)fail("xattr list");
        for(int w=0;w<3;w++){
            char name[64],prefix[48],value[80];snprintf(name,sizeof name,"user.infiltratorfs-rw-%d",w);
            bool present=false;for(ssize_t pos=0;pos<listed;pos+=(ssize_t)strlen(names+pos)+1)present|=!strcmp(names+pos,name);
            check(present,"missing xattr during contention");
            ssize_t got=getxattr(path,name,value,sizeof value);if(got<0)fail("xattr get");
            int length=snprintf(prefix,sizeof prefix,"writer-%d-iteration-",w);
            check(got>=length&&!memcmp(value,prefix,(size_t)length),"xattr reader value");
        }
    }}
}
static void open_unlink_worker(const char *dir,int w) {
    char name[64],first[64],second[64],result[128];
    snprintf(name,sizeof name,"open-unlink-%02d",w);char *path=join(dir,name);
    int fd=open(path,O_CREAT|O_EXCL|O_RDWR,0600);if(fd<0)fail("open unlink create");
    int a=snprintf(first,sizeof first,"before-%d\n",w),b=snprintf(second,sizeof second,"after-%d\n",w);
    write_all(fd,first,(size_t)a);if(fsync(fd)||unlink(path)||lseek(fd,0,SEEK_END)<0)fail("open unlink transition");
    write_all(fd,second,(size_t)b);if(fsync(fd)||lseek(fd,0,SEEK_SET)<0)fail("open unlink fsync");
    read_exact(fd,result,(size_t)(a+b));check(!memcmp(result,first,(size_t)a)&&!memcmp(result+a,second,(size_t)b),"orphan data");
    if(close(fd))fail("orphan close");
    check(access(path,F_OK)<0&&errno==ENOENT,"orphan path reappeared");free(path);
}
static void start_group(const char *phase,const char *path,void (*fn)(const char *,int),int count) {
    int gate[2];if(pipe(gate))fail("start pipe");pid_t children[WORKERS];
    for(int i=0;i<count;i++){pid_t pid=fork();if(pid<0)fail("fork worker");
        if(pid==0){close(gate[1]);char b;read_exact(gate[0],&b,1);close(gate[0]);fn(path,i);_exit(0);}children[i]=pid;}
    close(gate[0]);for(int i=0;i<count;i++)write_all(gate[1],"x",1);close(gate[1]);
    for(int i=0;i<count;i++){int status=0;time_t deadline=time(NULL)+120;
        while(waitpid(children[i],&status,WNOHANG)==0){if(time(NULL)>deadline){kill(children[i],SIGKILL);waitpid(children[i],&status,0);
                    fprintf(stderr,"%s worker %d timed out\n",phase,i);exit(1);}
            struct timespec pause={0,10000000};nanosleep(&pause,NULL);}
        if(!WIFEXITED(status)||WEXITSTATUS(status)){
            fprintf(stderr,"%s worker %d failed status=%d\n",phase,i,status);exit(1);}
    }
}
static void stable_reader(const char *path,int stop_fd) {
    int flags=fcntl(stop_fd,F_GETFL);if(flags<0||fcntl(stop_fd,F_SETFL,flags|O_NONBLOCK))fail("reader nonblock");
    unsigned char block[65536];int loops=0;
    for(;;){char stop;if(read(stop_fd,&stop,1)>=0)break;
        int fd=open(path,O_RDONLY);if(fd<0)fail("reader open");size_t offset=0;
        while(offset<2*REGION){read_exact(fd,block,sizeof block);
            for(size_t n=0;n<sizeof block;n++)check(block[n]==(unsigned char)(((offset+n)%REGION*29+5)&255),"stable reader corruption");
            offset+=sizeof block;}
        close(fd);loops++;}
    check(loops>0,"stable reader no reads");
}
static void write_semantics(const char *dir,bool skip_privilege) {
    char *sync=join(dir,"sync-flags.bin");
    int fd=open(sync,O_CREAT|O_EXCL|O_WRONLY|O_SYNC|O_DSYNC,0600);if(fd<0)fail("sync open");
    const char data[]="synchronous-write-contract";write_all(fd,data,sizeof data-1);
    if(close(fd))fail("sync close");
    fd=open(sync,O_RDONLY);if(fd<0)fail("sync read open");
    char readback[sizeof data];read_exact(fd,readback,sizeof data-1);check(!memcmp(readback,data,sizeof data-1),"sync readback");close(fd);free(sync);
    if(skip_privilege)return;
    char *priv=join(dir,"privilege-strip.bin");fd=open(priv,O_CREAT|O_EXCL|O_WRONLY,0600);if(fd<0||close(fd))fail("priv create");
    if(chown(priv,65534,65534)||chmod(priv,06755))fail("priv metadata");
    fd=open(priv,O_WRONLY|O_APPEND);if(fd<0)fail("priv open");
    pid_t pid=fork();if(pid<0)fail("priv fork");if(pid==0){if(setgroups(0,NULL)||setgid(65534)||setuid(65534))_exit(3);
        _exit(write(fd,"unprivileged-write",18)==18?0:2);}
    close(fd);int status;if(waitpid(pid,&status,0)<0)fail("priv wait");
    check(WIFEXITED(status)&&WEXITSTATUS(status)==0,"unprivileged write");
    struct stat st;if(stat(priv,&st))fail("priv stat");check(!(st.st_mode&(S_ISUID|S_ISGID)),"set-ID bits retained");free(priv);
}
int main(int argc,char **argv) {
    bool skip_privilege=argc==3&&!strcmp(argv[2],"--skip-privilege-smoke");
    if(argc!=2&&!skip_privilege){fprintf(stderr,"usage: %s MOUNTED_TEST_DIRECTORY [--skip-privilege-smoke]\n",argv[0]);return 2;}
    root=argv[1];if(mkdir(root,0700))fail("create qualification directory");
    char *stable=join(root,"stable-reader.bin");int fd=open(stable,O_CREAT|O_EXCL|O_WRONLY,0600);
    if(fd<0)fail("stable create");
    unsigned char pattern[65536];
    for(size_t i=0;i<sizeof pattern;i++)pattern[i]=(unsigned char)((i*29+5)&255);
    for(int i=0;i<32;i++)write_all(fd,pattern,sizeof pattern);
    if(fsync(fd)||close(fd))fail("stable fsync");
    int stop[2];if(pipe(stop))fail("reader stop pipe");pid_t reader=fork();if(reader<0)fail("reader fork");
    if(reader==0){close(stop[1]);stable_reader(stable,stop[0]);_exit(0);}close(stop[0]);
    char *dir=join(root,"shared-directory");if(mkdir(dir,0700))fail("namespace mkdir");
    start_group("namespace",dir,namespace_worker,WORKERS);verify_namespace(dir);
    char *target=join(dir,"w00-000.dat");for(int w=0;w<3;w++){
        char name[64],value[64];snprintf(name,sizeof name,"user.infiltratorfs-rw-%d",w);
        int n=snprintf(value,sizeof value,"writer-%d-iteration-000",w);
        if(setxattr(target,name,value,(size_t)n,0))fail("xattr initialize");}
    start_group("xattr",target,xattr_worker,WORKERS);
    for(int w=0;w<3;w++){char name[64],expected[64],actual[64];snprintf(name,sizeof name,"user.infiltratorfs-rw-%d",w);
        int n=snprintf(expected,sizeof expected,"writer-%d-iteration-127",w);
        check(getxattr(target,name,actual,sizeof actual)==n&&!memcmp(actual,expected,(size_t)n),"final xattr");}
    char *shared=join(root,"shared-file.bin");fd=open(shared,O_CREAT|O_EXCL|O_RDWR,0600);
    if(fd<0||ftruncate(fd,WORKERS*REGION)||fsync(fd)||close(fd))fail("shared create");
    start_group("shared",shared,shared_worker,WORKERS);verify_shared(shared);
    char *append=join(root,"atomic-append.bin");fd=open(append,O_CREAT|O_EXCL|O_WRONLY,0600);
    if(fd<0||close(fd))fail("append create");
    start_group("append",append,append_worker,WORKERS);verify_append(append);
    write_semantics(root,skip_privilege);
    start_group("open-unlink",root,open_unlink_worker,WORKERS);
    close(stop[1]);int status;if(waitpid(reader,&status,0)<0)fail("reader wait");
    check(WIFEXITED(status)&&WEXITSTATUS(status)==0,"stable reader status");
    int dfd=open(root,O_RDONLY|O_DIRECTORY);if(dfd<0||fsync(dfd)||close(dfd))fail("root fsync");
    puts(skip_privilege ? "Native concurrency smoke: PASS (privilege phase skipped)" :
         "Native concurrency qualification: PASS (6 mutators, 144 files, 6 shared-inode writers, "
         "3 xattr writers/readers, 1536 atomic append records, sync flags, privilege stripping, 6 open-unlink writers)");
    free(stable);free(dir);free(target);free(shared);free(append);return 0;
}
