// SPDX-License-Identifier: GPL-3.0-or-later
/* Format 0.19 durable-storage conformance: actual portable writer, volatile/durable
 * storage planes, failure at each issued write/flush, fresh graph+data scrub.
 * Disposable RAM only; this is not a native-kernel/power-loss qualification. */
#include "infilfs/volume.h"
#include "infilfs/check.h"
#include "infilfs/format_volume.h"
#include "infilfs/endian.h"
#include "infilfs/fs.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BYTES (32u * 1024u * 1024u)
#define SMALL_BYTES (16u * 1024u * 1024u)
#define PAYLOAD (64u * 1024u)
struct media {
    unsigned char *live, *durable;
    unsigned event, fail_at, mode, random_counter;
    uint64_t physical_bytes;
    int crashed, armed;
};
static void require(int ok, const char *what) {
    if (!ok) { fprintf(stderr, "PROBE FAIL: %s\n", what); exit(1); }
}
static infs_status rd(void *p, uint64_t off, void *out, size_t n) {
    struct media *m=p;
    if (m->crashed || off>BYTES || n>BYTES-off) return INFS_STATUS_IO_ERROR;
    memcpy(out,m->live+off,n); return INFS_STATUS_OK;
}
static infs_status wr(void *p, uint64_t off, const void *in, size_t n) {
    struct media *m=p;
    if (m->crashed || off>BYTES || n>BYTES-off) return INFS_STATUS_IO_ERROR;
    if (m->armed && ++m->event==m->fail_at) {
        if (m->mode) { /* torn write reaches stable storage before failure */
            size_t partial=n>64u?64u:n/2u;
            memcpy(m->durable+off,in,partial);
        }
        m->crashed=1; return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->live+off,in,n); return INFS_STATUS_OK;
}
static infs_status flush(void *p) {
    struct media *m=p;
    if (m->crashed) return INFS_STATUS_IO_ERROR;
    if (m->armed && ++m->event==m->fail_at) {
        if (m->mode) memcpy(m->durable,m->live,BYTES);
        m->crashed=1; return INFS_STATUS_IO_ERROR;
    }
    memcpy(m->durable,m->live,BYTES); return INFS_STATUS_OK;
}
static infs_status size(void *p,uint64_t *out,int *device) {
    struct media *m=p; *out=m->physical_bytes; *device=0; return INFS_STATUS_OK;
}
static infs_status random_bytes(void *p,void *out,size_t n) {
    struct media *m=p; memset(out,++m->random_counter,n); return INFS_STATUS_OK;
}
static infs_status now(void *p,struct infs_timestamp *t) {
    (void)p; memset(t,0,sizeof(*t)); t->seconds=1234567; return INFS_STATUS_OK;
}
static const struct infs_storage_ops ops={.read_at=rd,.write_at=wr,
    .flush=flush,.get_size=size,.random_bytes=random_bytes,.current_time=now};
static const struct infs_create_options options={.posix_permissions=0644};
static unsigned char payload[PAYLOAD], readback[PAYLOAD], seed[4096];
static void open_rw(struct media *m,struct infs_volume *v) {
    struct infs_storage s={.ops=&ops,.context=m};
    require(infs_volume_open_019_storage(v,&s,1)==INFS_STATUS_OK,"open writer");
}
static infs_status mutation(struct infs_volume *v) {
    infs_status s=infs_volume_set_deferred_publish(v,1,UINT64_C(1073741824));
    if (s==INFS_STATUS_OK) s=infs_create_file(v,"/pending",&options);
    if (s==INFS_STATUS_OK) {
        int64_t n=infs_write_file(v,"/pending",payload,PAYLOAD,0);
        if (n!=PAYLOAD) return INFS_STATUS_IO_ERROR;
        s=infs_volume_sync(v);
    }
    return s;
}
static void verify_crashed(struct media *m,int corrupt_primary) {
    struct infs_volume v={0};
    struct infs_storage s={.ops=&ops,.context=m};
    struct infs_scrub_report report={0};
    m->armed=0; m->crashed=0;
    memcpy(m->live,m->durable,BYTES);
    if (corrupt_primary) m->live[0]^=255u;
    require(infs_volume_open_019_test_image(&v,&s)==INFS_STATUS_OK,
            "fresh read-only complete-graph recovery");
    require(infs_read_file(&v,"/stable",readback,sizeof(seed),0)==sizeof(seed)
            && !memcmp(seed,readback,sizeof(seed)),"last committed stable data survives");
    struct infs_lookup lookup={0};
    infs_status status=infs_lookup_path(&v,"/pending",&lookup);
    if (status==INFS_STATUS_OK) {
        require(infs_read_file(&v,"/pending",readback,PAYLOAD,0)==PAYLOAD
                && !memcmp(payload,readback,PAYLOAD),"new transaction is complete, not partial");
    } else require(status==INFS_STATUS_NOT_FOUND,"new transaction absent or fully present");
    require(infs_scrub(&v,&report)==INFS_STATUS_OK && !report.metadata_errors
            && !report.checksum_errors,"recovered committed graph deep-scrubs clean");
    infs_volume_close(&v);
}
int main(void) {
    struct media m={0};
    struct infs_volume v={0};
    unsigned char *baseline=malloc(BYTES),*crash=malloc(BYTES);
    m.live=calloc(1,BYTES); m.durable=calloc(1,BYTES);
    m.physical_bytes=SMALL_BYTES;
    require(baseline&&crash&&m.live&&m.durable,"allocate disposable media");
    for (size_t i=0;i<PAYLOAD;i++) payload[i]=(unsigned char)(i*109u+(i>>9));
    memset(seed,0x5a,sizeof(seed));
    struct infs_storage s={.ops=&ops,.context=&m};
    require(infs_format_storage_019_test_image(&s,"CrashProbe")==INFS_STATUS_OK,"format baseline");
    open_rw(&m,&v);
    require(infs_create_file(&v,"/stable",&options)==INFS_STATUS_OK,"create seed");
    require(infs_write_file(&v,"/stable",seed,sizeof(seed),0)==sizeof(seed),"seed data");
    infs_volume_close(&v);
    m.physical_bytes=BYTES;
    memcpy(baseline,m.durable,BYTES);
    unsigned saved_random=m.random_counter;
    open_rw(&m,&v); m.armed=1;
    require(mutation(&v)==INFS_STATUS_OK,"no-fault transaction");
    unsigned events=m.event;
    m.armed=0; infs_volume_close(&v);
    unsigned cases=0;
    for (unsigned mode=0;mode<2;mode++) for (unsigned event=1;event<=events;event++) {
        memcpy(m.live,baseline,BYTES); memcpy(m.durable,baseline,BYTES);
        m.armed=0; m.crashed=0; m.event=0; m.fail_at=event;
        m.mode=mode; m.random_counter=saved_random;
        open_rw(&m,&v); m.armed=1;
        infs_status status=mutation(&v);
        require(m.crashed&&status!=INFS_STATUS_OK,"each selected storage event faulted");
        memcpy(crash,m.durable,BYTES);
        infs_volume_close(&v);
        memcpy(m.durable,crash,BYTES); verify_crashed(&m,0); cases++;
        memcpy(m.durable,crash,BYTES); verify_crashed(&m,1); cases++;
    }
    printf("Actual portable Format 0.19 crash probe: %u storage boundaries, %u recovery/scrub cases PASS\n",events,cases);
    memcpy(m.live,baseline,BYTES); memcpy(m.durable,baseline,BYTES);
    m.armed=0; m.crashed=0; m.random_counter=saved_random;
    open_rw(&m,&v);
    require(infs_snapshot_create(&v,"before")==INFS_STATUS_OK,"0.19 snapshot create");
    require(infs_reflink_file(&v,"/stable","/clone")==INFS_STATUS_OK,"0.19 reflink");
    memset(readback,0x6b,sizeof(seed));
    require(infs_write_file(&v,"/stable",readback,sizeof(seed),0)==sizeof(seed),"0.19 overwrite");
    require(infs_read_file(&v,"/clone",readback,sizeof(seed),0)==sizeof(seed)
            && !memcmp(seed,readback,sizeof(seed)),"reflink preserves old data");
    require(infs_snapshot_read_file(&v,"before","/stable",readback,sizeof(seed),0)==sizeof(seed)
            && !memcmp(seed,readback,sizeof(seed)),"0.19 snapshot retained data");
    require(infs_rename(&v,"/clone","/renamed")==INFS_STATUS_OK,"0.19 rename");
    require(infs_truncate_file(&v,"/renamed",1024)==INFS_STATUS_OK,"0.19 truncate");
    require(infs_unlink(&v,"/renamed")==INFS_STATUS_OK,"0.19 unlink");
    struct infs_scrub_report report={0};
    require(infs_scrub(&v,&report)==INFS_STATUS_OK && !report.metadata_errors
            && !report.checksum_errors,"0.19 snapshot and mutation deep scrub");
    require(infs_snapshot_rollback(&v,"before")==INFS_STATUS_OK,"0.19 rollback");
    require(infs_read_file(&v,"/stable",readback,sizeof(seed),0)==sizeof(seed)
            && !memcmp(seed,readback,sizeof(seed)),"rollback restores seed data");
    infs_volume_close(&v);
    struct infs_volume ro={0}; struct infs_storage rs={.ops=&ops,.context=&m};
    require(infs_volume_open_019_test_image(&ro,&rs)==INFS_STATUS_OK,"0.19 reopen after rollback");
    require(infs_scrub(&ro,&report)==INFS_STATUS_OK && !report.metadata_errors
            && !report.checksum_errors,"0.19 post-rollback scrub");
    infs_volume_close(&ro);
    puts("Portable 0.19 feature smoke: snapshot, reflink, overwrite, rename, truncate, unlink, rollback and reopen/scrub PASS");
    memcpy(m.live,baseline,BYTES); memcpy(m.durable,baseline,BYTES);
    struct infs_superblock_disk disk;
    memcpy(&disk,baseline,sizeof(disk));
    disk.ro_compat_flags=infs_cpu_to_le64(UINT64_C(0x8000000000000000));
    unsigned char page[INFS_BLOCK_SIZE];
    require(infs_encode_superblock(page,&disk)==INFS_STATUS_OK,"encode unknown RO-compatible feature");
    for (unsigned i=0;i<INFS_CHECKPOINT_COUNT;i++) {
        uint64_t block=infs_le64_to_cpu(disk.checkpoint_block[i]);
        memcpy(m.live+block*INFS_BLOCK_SIZE,page,INFS_BLOCK_SIZE);
        memcpy(m.durable+block*INFS_BLOCK_SIZE,page,INFS_BLOCK_SIZE);
    }
    struct infs_volume unsupported={0};
    struct infs_storage us={.ops=&ops,.context=&m};
    infs_status opened=infs_volume_open_019_storage(&unsupported,&us,1);
    infs_status mutated=opened==INFS_STATUS_OK ?
        infs_create_file(&unsupported,"/unknown-feature-write",&options) : opened;
    require(opened==INFS_STATUS_NOT_SUPPORTED && mutated==INFS_STATUS_NOT_SUPPORTED,
            "unknown RO-compatible feature must reject writable admission");
    puts("Format 0.19 unknown-feature writable admission: PASS");
    if (opened==INFS_STATUS_OK) infs_volume_close(&unsupported);
    us=(struct infs_storage){.ops=&ops,.context=&m};
    require(infs_volume_open_019_storage(&unsupported,&us,0)==INFS_STATUS_OK,
            "unknown RO-compatible feature permits read-only inspection");
    infs_volume_close(&unsupported);
    disk.ro_compat_flags=0;
    disk.format_minor=infs_cpu_to_le16(18u);
    require(infs_encode_superblock(page,&disk)==INFS_STATUS_OK,"encode superseded format");
    memcpy(m.live,baseline,BYTES);
    for (unsigned i=0;i<INFS_CHECKPOINT_COUNT;i++) {
        uint64_t block=infs_le64_to_cpu(disk.checkpoint_block[i]);
        memcpy(m.live+block*INFS_BLOCK_SIZE,page,INFS_BLOCK_SIZE);
    }
    memset(m.live+INFS_BLOCK_SIZE,0,INFS_BLOCK_SIZE);
    memset(m.live+2047u*INFS_BLOCK_SIZE,0,INFS_BLOCK_SIZE);
    memcpy(crash,m.live,BYTES);
    us=(struct infs_storage){.ops=&ops,.context=&m};
    require(infs_volume_open_storage(&unsupported,&us,1)!=INFS_STATUS_OK,
            "normal open rejects superseded Format 0.18");
    require(!memcmp(m.live,crash,BYTES),"old-format refusal leaves media unchanged");
    puts("Format 0.18 rejection without mutation: PASS");
    unsigned resize_cases=0;
    for (unsigned shrinking=0; shrinking<2u; ++shrinking) {
        memcpy(m.live,baseline,BYTES); memcpy(m.durable,baseline,BYTES);
        m.armed=0; m.crashed=0; m.fail_at=0; m.event=0;
        m.random_counter=saved_random;
        open_rw(&m,&v);
        if (shrinking) {
            require(infs_volume_resize(&v,BYTES)==INFS_STATUS_OK,"prepare grown geometry");
            infs_volume_close(&v);
            memcpy(baseline,m.durable,BYTES);
            open_rw(&m,&v);
        }
        uint64_t target=shrinking?SMALL_BYTES:BYTES;
        m.armed=1; m.event=0;
        require(infs_volume_resize(&v,target)==INFS_STATUS_OK,"successful 0.19 resize");
        unsigned boundaries=m.event;
        m.armed=0; infs_volume_close(&v);
        memcpy(crash,m.durable,BYTES);
        verify_crashed(&m,1);
        if (shrinking) {
            m.physical_bytes=SMALL_BYTES;
            memcpy(m.durable,crash,BYTES);
            verify_crashed(&m,1);
            m.physical_bytes=BYTES;
        }
        for (unsigned mode=0; mode<2u; ++mode)
        for (unsigned event=1; event<=boundaries; ++event) {
            memcpy(m.live,baseline,BYTES); memcpy(m.durable,baseline,BYTES);
            m.armed=0; m.crashed=0; m.event=0; m.fail_at=event; m.mode=mode;
            open_rw(&m,&v); m.armed=1;
            infs_status failed=infs_volume_resize(&v,target);
            require(failed!=INFS_STATUS_OK && m.crashed,"resize interruption is reported");
            memcpy(crash,m.durable,BYTES); infs_volume_close(&v);
            for (unsigned primary_loss=0; primary_loss<2u; ++primary_loss) {
                memcpy(m.durable,crash,BYTES);
                verify_crashed(&m,primary_loss); ++resize_cases;
                memcpy(m.live,m.durable,BYTES);
                if (primary_loss) m.live[0]^=255u;
                m.armed=0; m.crashed=0;
                open_rw(&m,&v);
                require(infs_create_file(&v,"/resumed",&options)==INFS_STATUS_OK,
                        "resume writes after interrupted geometry recovery");
                struct infs_check_report check={0};
                require(infs_check(&v,&check)==INFS_STATUS_OK &&
                        check.checkpoint_replicas_valid==3u &&
                        check.recovery_locators_valid==2u,"repair all discovery redundancy");
                infs_volume_close(&v);
            }
        }
        printf("Format 0.19 %s: %u storage interruption boundaries PASS\n",
               shrinking?"shrink":"grow",boundaries);
    }
    printf("Format 0.19 actual resize crash/recovery/repair: %u cases PASS\n",resize_cases);
    free(m.live); free(m.durable); free(baseline); free(crash);
    return 0;
}
