// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/format_volume.h"
#include "infilfs/storage_encrypted.h"
#include "infilfs/storage_mirror.h"
#include "infilfs/volume.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MEMBER_BYTES (32u * 1024u * 1024u)

struct memory_member {
    uint8_t *bytes;
    size_t size;
    uint64_t rng;
    int fail_reads;
    int fail_writes;
};

static void fail(const char *message)
{
    fprintf(stderr, "protected-volume: %s\n", message);
    exit(1);
}

static void ok(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status rd(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    struct memory_member *m = opaque;
    if (m->fail_reads)
        return INFS_STATUS_IO_ERROR;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, m->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status wr(void *opaque, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct memory_member *m = opaque;
    if (m->fail_writes)
        return INFS_STATUS_IO_ERROR;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(m->bytes + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status fl(void *opaque)
{
    struct memory_member *m = opaque;
    return m->fail_writes ? INFS_STATUS_IO_ERROR : INFS_STATUS_OK;
}

static infs_status gs(void *opaque, uint64_t *bytes, int *is_device)
{
    struct memory_member *m = opaque;
    *bytes = m->size;
    *is_device = 1;
    return INFS_STATUS_OK;
}

static infs_status rnd(void *opaque, void *buffer, size_t size)
{
    struct memory_member *m = opaque;
    uint8_t *out = buffer;
    for (size_t i = 0; i < size; ++i) {
        m->rng ^= m->rng << 13;
        m->rng ^= m->rng >> 7;
        m->rng ^= m->rng << 17;
        out[i] = (uint8_t)m->rng;
    }
    return INFS_STATUS_OK;
}

static infs_status now(void *opaque, struct infs_timestamp *time)
{
    (void)opaque;
    time->seconds = 1786752000;
    time->nanoseconds = 0;
    return INFS_STATUS_OK;
}

static void cls(void *opaque)
{
    (void)opaque;
}

static const struct infs_storage_ops ops = {
    .read_at = rd,
    .write_at = wr,
    .flush = fl,
    .get_size = gs,
    .random_bytes = rnd,
    .current_time = now,
    .close = cls,
};

static struct infs_storage member_storage(struct memory_member *m)
{
    struct infs_storage storage = { .ops = &ops, .context = m };
    return storage;
}

static infs_status build_authenticated_mirror(
    struct memory_member member[2], const char *passphrase, int format,
    struct infs_storage *out)
{
    struct infs_storage encrypted[2] = {{0}};
    for (size_t i = 0; i < 2u; ++i) {
        struct infs_storage backing = member_storage(&member[i]);
        infs_status status = format ?
            infs_storage_encrypted_format(
                &backing, passphrase, strlen(passphrase), 100000u,
                &encrypted[i]) :
            infs_storage_encrypted_open(
                &backing, passphrase, strlen(passphrase), &encrypted[i]);
        if (status != INFS_STATUS_OK) {
            for (size_t n = 0; n < i; ++n)
                infs_storage_close(&encrypted[n]);
            return status;
        }
    }
    infs_status status = infs_storage_mirror_create(encrypted, 2u, out);
    if (status != INFS_STATUS_OK) {
        for (size_t i = 0; i < 2u; ++i)
            infs_storage_close(&encrypted[i]);
    }
    return status;
}

int main(void)
{
    struct memory_member member[2] = {
        {calloc(1, MEMBER_BYTES), MEMBER_BYTES,
         UINT64_C(0x123456789abcdef0), 0, 0},
        {calloc(1, MEMBER_BYTES), MEMBER_BYTES,
         UINT64_C(0xfedcba9876543210), 0, 0},
    };
    ok(member[0].bytes && member[1].bytes, "allocate members");

    const char *passphrase = "authenticated mirrored InfiltratorFS";
    struct infs_storage protected_storage = {0};
    infs_status status = build_authenticated_mirror(
        member, passphrase, 1, &protected_storage);
    if (status == INFS_STATUS_NOT_SUPPORTED) {
        free(member[0].bytes);
        free(member[1].bytes);
        puts("protected-volume: SKIP (crypto provider unavailable)");
        return 0;
    }
    ok(status == INFS_STATUS_OK, "create encrypted members and mirror");

    ok(infs_format_storage(&protected_storage, "protected") ==
           INFS_STATUS_OK,
       "format filesystem through authenticated mirror");

    struct infs_volume volume;
    ok(infs_volume_open_storage(
           &volume, &protected_storage, 1) == INFS_STATUS_OK,
       "open protected filesystem");
    struct infs_create_options options = { .posix_permissions = 0644 };
    ok(infs_create_file(&volume, "/protected.txt", &options) ==
           INFS_STATUS_OK,
       "create protected file");

    uint8_t payload[16384];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)(i * 29u + 7u);
    ok(infs_write_file(
           &volume, "/protected.txt", payload, sizeof(payload), 0) ==
           (int64_t)sizeof(payload),
       "write protected file");
    ok(infs_snapshot_create(&volume, "protected-baseline") ==
           INFS_STATUS_OK,
       "snapshot protected filesystem");
    ok(infs_volume_sync(&volume) == INFS_STATUS_OK,
       "sync protected filesystem");
    infs_volume_close(&volume);

    /*
     * Reopen both authenticated members, then fail all reads from the first
     * backing.  The mirror must receive an I/O/authentication failure from that
     * member and satisfy the filesystem read from the second complete replica.
     */
    struct infs_storage encrypted[2] = {{0}};
    for (size_t i = 0; i < 2u; ++i) {
        struct infs_storage backing = member_storage(&member[i]);
        ok(infs_storage_encrypted_open(
               &backing, passphrase, strlen(passphrase), &encrypted[i]) ==
               INFS_STATUS_OK,
           "reopen encrypted member");
    }
    member[0].fail_reads = 1;
    ok(infs_storage_mirror_create(encrypted, 2u, &protected_storage) ==
           INFS_STATUS_OK,
       "rebuild authenticated mirror");
    ok(infs_volume_open_storage(
           &volume, &protected_storage, 0) == INFS_STATUS_OK,
       "open through surviving authenticated replica");

    uint8_t readback[sizeof(payload)];
    memset(readback, 0, sizeof(readback));
    ok(infs_read_file(
           &volume, "/protected.txt", readback, sizeof(readback), 0) ==
           (int64_t)sizeof(readback),
       "read through surviving replica");
    ok(!memcmp(readback, payload, sizeof(payload)),
       "replica data matches");
    struct infs_scrub_report report;
    ok(infs_scrub(&volume, &report) == INFS_STATUS_OK &&
           report.metadata_errors == 0 && report.checksum_errors == 0,
       "scrub protected filesystem");
    infs_volume_close(&volume);

    member[0].fail_reads = 0;
    free(member[0].bytes);
    free(member[1].bytes);
    puts("protected-volume: PASS");
    return 0;
}
