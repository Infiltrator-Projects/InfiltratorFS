// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format_volume.h"
#include "infilfs/fs.h"
#include "infilfs/storage.h"
#include "infilfs/volume.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct image { unsigned char *bytes; size_t size; uint64_t random_state; };
static infs_status rd(void *c, uint64_t o, void *b, size_t s) {
    struct image *i=c; if (o>i->size || s>i->size-(size_t)o) return INFS_STATUS_IO_ERROR;
    memcpy(b,i->bytes+(size_t)o,s); return INFS_STATUS_OK;
}
static infs_status wr(void *c, uint64_t o, const void *b, size_t s) {
    struct image *i=c; if (o>i->size || s>i->size-(size_t)o) return INFS_STATUS_IO_ERROR;
    memcpy(i->bytes+(size_t)o,b,s); return INFS_STATUS_OK;
}
static infs_status fl(void *c){(void)c; return INFS_STATUS_OK;}
static infs_status sz(void *c, uint64_t *bytes, int *is_device) {
    struct image *i=c; *bytes=i->size; *is_device=0; return INFS_STATUS_OK;
}
static infs_status rnd(void *c, void *b, size_t s) {
    struct image *i=c; unsigned char *out=b;
    for (size_t n=0;n<s;++n) { i->random_state ^= i->random_state << 13; i->random_state ^= i->random_state >> 7; i->random_state ^= i->random_state << 17; out[n]=(unsigned char)i->random_state; }
    return INFS_STATUS_OK;
}
static infs_status now(void *c, struct infs_timestamp *t) {
    (void)c; t->seconds=1788249600; t->nanoseconds=0; return INFS_STATUS_OK;
}
static void cls(void *c){(void)c;}
static const struct infs_storage_ops ops = {
    .read_at=rd, .write_at=wr, .flush=fl, .get_size=sz,
    .random_bytes=rnd, .current_time=now, .close=cls,
};
static struct infs_storage make_storage(struct image *i) {
    struct infs_storage st={ .ops=&ops, .context=i }; return st;
}
static void expect(int ok, const char *m){if(!ok){fprintf(stderr,"FAIL: %s\n",m);exit(1);}}

int main(void) {
    struct image image = { calloc(1, 64u*1024u*1024u), 64u*1024u*1024u, UINT64_C(0x9e3779b97f4a7c15) };
    expect(image.bytes != NULL, "allocate");
    struct infs_storage st = make_storage(&image);
    expect(infs_format_storage(&st, "compression-metrics") == INFS_STATUS_OK, "format");
    struct infs_volume vol;
    st = make_storage(&image);
    expect(infs_volume_open_storage(&vol,&st,1) == INFS_STATUS_OK, "open");
    expect(infs_create_file(&vol,"/data",NULL) == INFS_STATUS_OK, "create");
    size_t n=4u*1024u*1024u;
    unsigned char *buf=malloc(n); expect(buf!=NULL,"buffer");
    for(size_t i=0;i<n;++i) buf[i]=(unsigned char)("AAAAAAAABBBBBBBB"[i&15]);
    expect(infs_write_file(&vol,"/data",buf,n,0) == (int64_t)n, "write");
    struct infs_compression_metrics one;
    expect(infs_compression_metrics(&vol,&one) == INFS_STATUS_OK, "metrics one");
    expect(one.compression_saved_bytes > 0, "saved bytes");
    expect(one.unique_compressed_physical_bytes < one.unique_compressed_logical_bytes,
           "physical less logical");
    expect(infs_reflink_file(&vol,"/data","/clone") == INFS_STATUS_OK, "reflink");
    expect(infs_snapshot_create(&vol,"retained") == INFS_STATUS_OK, "snapshot");
    struct infs_compression_metrics shared;
    expect(infs_compression_metrics(&vol,&shared) == INFS_STATUS_OK, "shared metrics");
    expect(shared.snapshots_scanned == 1, "snapshot scanned");
    expect(shared.compressed_referenced_logical_bytes >
           one.compressed_referenced_logical_bytes, "references grow");
    expect(shared.compression_saved_bytes == one.compression_saved_bytes,
           "unique physical compression saving deduped");
    free(buf);
    infs_volume_close(&vol);
    free(image.bytes);
    puts("Compression metrics: PASS");
    return 0;
}
