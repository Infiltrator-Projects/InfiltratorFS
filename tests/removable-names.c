// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/endian.h"
#include "infilfs/format_volume.h"
#include "infilfs/volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_SIZE (32u * 1024u * 1024u)

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
    t->seconds = INT64_C(1786744800);
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

static void expect(int ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "removable-names: %s\n", what);
        exit(1);
    }
}

int main(void)
{
    struct memory_storage m = {
        .bytes = calloc(1, IMAGE_SIZE),
        .size = IMAGE_SIZE,
        .random_state = UINT64_C(0x123456789abcdef0),
    };
    expect(m.bytes != NULL, "allocate image");

    struct infs_storage s = storage_for(&m);
    expect(infs_format_storage_with_options(
               &s, "portable", INFS_FORMAT_OPTION_REMOVABLE_NAMES_V1) ==
               INFS_STATUS_OK,
           "format profile volume");

    s = storage_for(&m);
    struct infs_volume v;
    expect(infs_volume_open_storage(&v, &s, 1) == INFS_STATUS_OK,
           "open profile volume");
    expect((infs_le64_to_cpu(v.sb.incompat_flags) &
            INFS_INCOMPAT_REMOVABLE_NAMES_V1) != 0,
           "profile bit persisted");

    const struct infs_create_options create = { .posix_permissions = 0644 };
    expect(infs_create_file(&v, "/resume-Ã©.txt", &create) ==
               INFS_STATUS_OK,
           "accept portable Unicode name");
    expect(infs_create_file(&v, "/com10.txt", &create) == INFS_STATUS_OK,
           "accept non-reserved device-like name");

    const char *bad[] = {
        "/CON", "/con.txt", "/PRN.doc", "/NUL", "/COM1.log", "/LPT9",
        "/bad:name", "/bad?name", "/bad*name", "/bad|name", "/bad<name",
        "/bad>name", "/bad\"name", "/trail.", "/trail ", "/control-"
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        expect(infs_create_file(&v, bad[i], &create) ==
                   INFS_STATUS_INVALID_ARGUMENT,
               "reject non-portable component");

    char long_name[258];
    long_name[0] = '/';
    memset(long_name + 1, 'a', 256);
    long_name[257] = '\0';
    expect(infs_create_file(&v, long_name, &create) ==
               INFS_STATUS_INVALID_ARGUMENT,
           "reject component above 255 bytes in profile");

    expect(infs_volume_sync(&v) == INFS_STATUS_OK, "publish profile names");
    infs_volume_close(&v);

    s = storage_for(&m);
    expect(infs_volume_open_storage(&v, &s, 0) == INFS_STATUS_OK,
           "reopen profile volume");
    struct infs_lookup found;
    expect(infs_lookup_path(&v, "/resume-Ã©.txt", &found) ==
               INFS_STATUS_OK,
           "portable Unicode name survives reopen");
    infs_volume_close(&v);

    free(m.bytes);
    puts("removable-names: PASS");
    return 0;
}
