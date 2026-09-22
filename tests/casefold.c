// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE (64u * 1024u * 1024u)

struct memory_storage {
    uint8_t *bytes;
    size_t size;
    uint64_t random_state;
};

static infs_status mem_read(void *ctx, uint64_t off, void *buf, size_t n)
{
    struct memory_storage *m = ctx;
    if (off > m->size || n > m->size - (size_t)off)
        return INFS_STATUS_IO_ERROR;
    memcpy(buf, m->bytes + (size_t)off, n);
    return INFS_STATUS_OK;
}

static infs_status mem_write(void *ctx, uint64_t off, const void *buf, size_t n)
{
    struct memory_storage *m = ctx;
    if (off > m->size || n > m->size - (size_t)off)
        return INFS_STATUS_IO_ERROR;
    memcpy(m->bytes + (size_t)off, buf, n);
    return INFS_STATUS_OK;
}

static infs_status mem_flush(void *ctx) { (void)ctx; return INFS_STATUS_OK; }

static infs_status mem_size(void *ctx, uint64_t *n, int *device)
{
    struct memory_storage *m = ctx;
    *n = m->size;
    *device = 0;
    return INFS_STATUS_OK;
}

static infs_status mem_random(void *ctx, void *buf, size_t n)
{
    struct memory_storage *m = ctx;
    uint8_t *out = buf;
    for (size_t i = 0; i < n; ++i) {
        m->random_state ^= m->random_state << 13;
        m->random_state ^= m->random_state >> 7;
        m->random_state ^= m->random_state << 17;
        out[i] = (uint8_t)m->random_state;
    }
    return INFS_STATUS_OK;
}

static infs_status mem_time(void *ctx, struct infs_timestamp *t)
{
    (void)ctx;
    t->seconds = INT64_C(1786748400);
    t->nanoseconds = 0;
    return INFS_STATUS_OK;
}

static void mem_close(void *ctx) { (void)ctx; }

static const struct infs_storage_ops ops = {
    .read_at = mem_read,
    .write_at = mem_write,
    .flush = mem_flush,
    .get_size = mem_size,
    .random_bytes = mem_random,
    .current_time = mem_time,
    .close = mem_close,
};

static struct infs_storage storage_for(struct memory_storage *m)
{
    struct infs_storage s = { .ops = &ops, .context = m };
    return s;
}

static void expect(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "casefold: %s\n", message);
        exit(1);
    }
}

static void reset_image(struct memory_storage *m)
{
    memset(m->bytes, 0, m->size);
    m->random_state = UINT64_C(0x123456789abcdef0);
}

int main(void)
{
    struct memory_storage m = {
        .bytes = calloc(1, IMAGE_SIZE),
        .size = IMAGE_SIZE,
        .random_state = UINT64_C(0x123456789abcdef0),
    };
    expect(m.bytes != NULL, "allocate image");
    const struct infs_create_options create = { .posix_permissions = 0644 };

    struct infs_storage s = storage_for(&m);
    expect(infs_format_storage(&s, "case-sensitive") == INFS_STATUS_OK,
           "format default volume");
    s = storage_for(&m);
    struct infs_volume v;
    expect(infs_volume_open_storage(&v, &s, 1) == INFS_STATUS_OK,
           "open default volume");
    expect((infs_le64_to_cpu(v.sb.incompat_flags) &
            INFS_INCOMPAT_CASEFOLD_V1) == 0,
           "casefold remains opt-in");
    expect(infs_create_file(&v, "/ReadMe", &create) == INFS_STATUS_OK,
           "create default mixed-case name");
    expect(infs_create_file(&v, "/README", &create) == INFS_STATUS_OK,
           "default volume keeps case-distinct names");
    infs_volume_close(&v);

    reset_image(&m);
    s = storage_for(&m);
    expect(infs_format_storage_with_options(
               &s, "casefold", INFS_FORMAT_OPTION_CASEFOLD_V1) ==
               INFS_STATUS_OK,
           "format casefold volume");
    s = storage_for(&m);
    expect(infs_volume_open_storage(&v, &s, 1) == INFS_STATUS_OK,
           "open casefold volume");
    expect((infs_le64_to_cpu(v.sb.incompat_flags) &
            INFS_INCOMPAT_CASEFOLD_V1) != 0,
           "casefold feature bit persisted");

    expect(infs_create_file(&v, "/ReadMe", &create) == INFS_STATUS_OK,
           "create mixed-case name");
    struct infs_lookup original, folded;
    expect(infs_lookup_path(&v, "/ReadMe", &original) == INFS_STATUS_OK,
           "lookup original case");
    expect(infs_lookup_path(&v, "/readme", &folded) == INFS_STATUS_OK &&
           memcmp(original.object_id, folded.object_id, 16) == 0,
           "case variant resolves same object");
    expect(infs_create_file(&v, "/README", &create) ==
               INFS_STATUS_ALREADY_EXISTS,
           "reject case-fold duplicate");

    static const char upper_non_ascii[] = "/\xc3\x84.txt";
    static const char lower_non_ascii[] = "/\xc3\xa4.txt";
    expect(infs_create_file(&v, upper_non_ascii, &create) == INFS_STATUS_OK,
           "create non-ASCII upper name");
    expect(infs_create_file(&v, lower_non_ascii, &create) == INFS_STATUS_OK,
           "non-ASCII bytes remain distinct in v1");

    for (unsigned i = 0; i < 320u; ++i) {
        char path[64];
        snprintf(path, sizeof(path), "/Entry-%03u-MixedCase", i);
        expect(infs_create_file(&v, path, &create) == INFS_STATUS_OK,
               "populate casefold directory tree");
    }
    for (unsigned i = 0; i < 320u; i += 31u) {
        char path[64];
        snprintf(path, sizeof(path), "/entry-%03u-mixedcase", i);
        expect(infs_lookup_path(&v, path, &folded) == INFS_STATUS_OK,
               "folded scalable-tree lookup");
    }

    struct infs_scrub_report report;
    expect(infs_scrub(&v, &report) == INFS_STATUS_OK &&
           report.metadata_errors == 0,
           "scrub casefold namespace");
    expect(infs_volume_sync(&v) == INFS_STATUS_OK, "publish casefold volume");
    infs_volume_close(&v);

    s = storage_for(&m);
    expect(infs_volume_open_storage(&v, &s, 0) == INFS_STATUS_OK,
           "reopen casefold volume");
    expect(infs_lookup_path(&v, "/rEaDmE", &folded) == INFS_STATUS_OK &&
           memcmp(original.object_id, folded.object_id, 16) == 0,
           "folded identity survives reopen");
    infs_volume_close(&v);

    free(m.bytes);
    puts("casefold: PASS");
    return 0;
}
