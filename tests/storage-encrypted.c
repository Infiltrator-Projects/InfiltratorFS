// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_encrypted.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define BACKING_BYTES (4096u + 32u * 4124u)

struct memory_backing {
    uint8_t *bytes;
    size_t size;
    uint64_t rng;
};

static void fail(const char *message)
{
    fprintf(stderr, "storage-encrypted: %s\n", message);
    exit(1);
}

static void ok(int condition, const char *message)
{
    if (!condition)
        fail(message);
}

static infs_status rd(void *opaque, uint64_t offset, void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(buffer, m->bytes + (size_t)offset, size);
    return INFS_STATUS_OK;
}

static infs_status wr(void *opaque, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
    memcpy(m->bytes + (size_t)offset, buffer, size);
    return INFS_STATUS_OK;
}

static infs_status fl(void *opaque)
{
    (void)opaque;
    return INFS_STATUS_OK;
}

static infs_status sz(void *opaque, uint64_t *bytes, int *is_device)
{
    struct memory_backing *m = opaque;
    *bytes = m->size;
    *is_device = 0;
    return INFS_STATUS_OK;
}

static infs_status rnd(void *opaque, void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
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
    time->seconds = 42;
    time->nanoseconds = 7;
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
    .get_size = sz,
    .random_bytes = rnd,
    .current_time = now,
    .close = cls,
};

static struct infs_storage storage(struct memory_backing *m)
{
    struct infs_storage s = { .ops = &ops, .context = m };
    return s;
}

int main(void)
{
    struct memory_backing memory = {
        .bytes = calloc(1, BACKING_BYTES),
        .size = BACKING_BYTES,
        .rng = UINT64_C(0x243f6a8885a308d3),
    };
    ok(memory.bytes != NULL, "allocate backing");

    static const char passphrase[] = "correct horse battery staple";
    struct infs_storage backing = storage(&memory);
    struct infs_storage encrypted = {0};
    infs_status status = infs_storage_encrypted_format(
        &backing, passphrase, sizeof(passphrase) - 1u, 100000u, &encrypted);
    if (status == INFS_STATUS_NOT_SUPPORTED) {
        free(memory.bytes);
        puts("authenticated encrypted storage: SKIP (crypto provider unavailable)");
        return 0;
    }
    ok(status == INFS_STATUS_OK, "format encrypted storage");
    ok(!backing.ops, "format transfers backing ownership");

    uint64_t logical_size = 0;
    int is_device = 1;
    ok(infs_storage_get_size(&encrypted, &logical_size, &is_device) ==
           INFS_STATUS_OK &&
       logical_size == 32u * 4096u && !is_device,
       "encrypted logical geometry");

    uint8_t payload[6000];
    for (size_t i = 0; i < sizeof(payload); ++i)
        payload[i] = (uint8_t)(i * 37u + 11u);
    ok(infs_storage_write(&encrypted, 3000, payload, sizeof(payload)) ==
           INFS_STATUS_OK,
       "unaligned encrypted write");
    ok(infs_storage_flush(&encrypted) == INFS_STATUS_OK,
       "encrypted durability barrier");

    uint8_t readback[sizeof(payload)];
    memset(readback, 0, sizeof(readback));
    ok(infs_storage_read(&encrypted, 3000, readback, sizeof(readback)) ==
           INFS_STATUS_OK &&
       !memcmp(readback, payload, sizeof(payload)),
       "encrypted round trip");
    infs_storage_close(&encrypted);

    backing = storage(&memory);
    static const char wrong[] = "wrong password";
    ok(infs_storage_encrypted_open(
           &backing, wrong, sizeof(wrong) - 1u, &encrypted) ==
           INFS_STATUS_CORRUPT,
       "wrong passphrase rejected");
    ok(backing.ops != NULL, "failed open does not steal backing");

    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_OK,
       "reopen encrypted storage");
    memset(readback, 0, sizeof(readback));
    ok(infs_storage_read(&encrypted, 3000, readback, sizeof(readback)) ==
           INFS_STATUS_OK &&
       !memcmp(readback, payload, sizeof(payload)),
       "encrypted data survives reopen");
    infs_storage_close(&encrypted);

    /*
     * Ciphertext starts after the 4 KiB envelope. Corrupt a non-zero record;
     * GCM must fail before unauthenticated bytes can reach the filesystem.
     */
    memory.bytes[4096u + 12u + 16u + 100u] ^= 0x80u;
    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_OK,
       "reopen for tamper test");
    uint8_t block[4096];
    ok(infs_storage_read(&encrypted, 0, block, sizeof(block)) ==
           INFS_STATUS_CORRUPT,
       "ciphertext tamper rejected");
    infs_storage_close(&encrypted);

    /*
     * Erasing a ciphertext record must also fail authentication.  There is no
     * unauthenticated all-zero representation for sparse/free logical blocks.
     */
    const size_t erased_record =
        4096u + 5u * (4096u + 12u + 16u);
    memset(memory.bytes + erased_record, 0, 4096u + 12u + 16u);
    backing = storage(&memory);
    ok(infs_storage_encrypted_open(
           &backing, passphrase, sizeof(passphrase) - 1u, &encrypted) ==
           INFS_STATUS_OK,
       "reopen for erased-record tamper test");
    ok(infs_storage_read(&encrypted, 5u * 4096u, block, sizeof(block)) ==
           INFS_STATUS_CORRUPT,
       "erased ciphertext record rejected");
    infs_storage_close(&encrypted);

    free(memory.bytes);
    puts("authenticated encrypted storage: PASS");
    return 0;
}
