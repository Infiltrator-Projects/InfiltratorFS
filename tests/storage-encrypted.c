// SPDX-License-Identifier: GPL-3.0-or-later
#include "infilfs/storage_encrypted.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <pthread.h>
#include <time.h>
#endif

#define BACKING_BYTES (4096u + 32u * 4124u)

struct memory_backing {
    uint8_t *bytes;
    size_t size;
    uint64_t rng;
#if !defined(_WIN32)
    pthread_mutex_t io_lock;
    pthread_mutex_t race_lock;
    pthread_cond_t race_cond;
    int race_mode;
    unsigned race_reads;
#endif
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
#if !defined(_WIN32)
    pthread_mutex_lock(&m->io_lock);
#endif
    memcpy(buffer, m->bytes + (size_t)offset, size);
#if !defined(_WIN32)
    pthread_mutex_unlock(&m->io_lock);

    /*
     * Deterministically expose the old partial-write race.  Two unsynchronised
     * encrypted writers both reach this point after decrypting the same old
     * record.  The repaired wrapper serialises by logical-block stripe, so the
     * first writer times out here and the second cannot read until that write
     * has completed.
     */
    pthread_mutex_lock(&m->race_lock);
    if (m->race_mode && offset >= 4096u) {
        ++m->race_reads;
        if (m->race_reads >= 2u) {
            m->race_mode = 0;
            pthread_cond_broadcast(&m->race_cond);
        } else {
            struct timespec deadline;
            timespec_get(&deadline, TIME_UTC);
            deadline.tv_nsec += 100000000L;
            if (deadline.tv_nsec >= 1000000000L) {
                ++deadline.tv_sec;
                deadline.tv_nsec -= 1000000000L;
            }
            while (m->race_mode)
                if (pthread_cond_timedwait(
                        &m->race_cond, &m->race_lock, &deadline) != 0)
                    break;
        }
    }
    pthread_mutex_unlock(&m->race_lock);
#endif
    return INFS_STATUS_OK;
}

static infs_status wr(void *opaque, uint64_t offset,
                      const void *buffer, size_t size)
{
    struct memory_backing *m = opaque;
    if (offset > m->size || size > m->size - (size_t)offset)
        return INFS_STATUS_IO_ERROR;
#if !defined(_WIN32)
    pthread_mutex_lock(&m->io_lock);
#endif
    memcpy(m->bytes + (size_t)offset, buffer, size);
#if !defined(_WIN32)
    pthread_mutex_unlock(&m->io_lock);
#endif
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
#if !defined(_WIN32)
    pthread_mutex_lock(&m->io_lock);
#endif
    for (size_t i = 0; i < size; ++i) {
        m->rng ^= m->rng << 13;
        m->rng ^= m->rng >> 7;
        m->rng ^= m->rng << 17;
        out[i] = (uint8_t)m->rng;
    }
#if !defined(_WIN32)
    pthread_mutex_unlock(&m->io_lock);
#endif
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

#if !defined(_WIN32)
struct partial_write {
    struct infs_storage *storage;
    uint64_t offset;
    uint8_t value;
    infs_status status;
};

static void *partial_write_thread(void *opaque)
{
    struct partial_write *write = opaque;
    uint8_t bytes[2048];
    memset(bytes, write->value, sizeof(bytes));
    write->status = infs_storage_write(
        write->storage, write->offset, bytes, sizeof(bytes));
    return NULL;
}
#endif

int main(void)
{
    struct memory_backing memory = {
        .bytes = calloc(1, BACKING_BYTES),
        .size = BACKING_BYTES,
        .rng = UINT64_C(0x243f6a8885a308d3),
    };
    ok(memory.bytes != NULL, "allocate backing");
#if !defined(_WIN32)
    ok(pthread_mutex_init(&memory.io_lock, NULL) == 0,
       "initialise backing I/O lock");
    ok(pthread_mutex_init(&memory.race_lock, NULL) == 0,
       "initialise race lock");
    ok(pthread_cond_init(&memory.race_cond, NULL) == 0,
       "initialise race condition");
#endif

    static const char passphrase[] = "correct horse battery staple";
    struct infs_storage backing = storage(&memory);
    struct infs_storage encrypted = {0};
    infs_status status = infs_storage_encrypted_format(
        &backing, passphrase, sizeof(passphrase) - 1u, 100000u, &encrypted);
    if (status == INFS_STATUS_NOT_SUPPORTED) {
#if !defined(_WIN32)
        pthread_cond_destroy(&memory.race_cond);
        pthread_mutex_destroy(&memory.race_lock);
        pthread_mutex_destroy(&memory.io_lock);
#endif
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

#if !defined(_WIN32)
    /*
     * Two disjoint partial updates to one authenticated logical block must be
     * atomic with respect to one another.  Without stripe locking, both
     * threads decrypt the same old block and one half-update is lost.
     */
    const uint64_t race_block = 12u * 4096u;
    uint8_t zero_block[4096] = {0};
    ok(infs_storage_write(&encrypted, race_block,
                          zero_block, sizeof(zero_block)) == INFS_STATUS_OK,
       "prepare concurrent partial-write block");
    pthread_mutex_lock(&memory.race_lock);
    memory.race_reads = 0;
    memory.race_mode = 1;
    pthread_mutex_unlock(&memory.race_lock);

    struct partial_write first = {
        .storage = &encrypted, .offset = race_block,
        .value = 0x3c, .status = INFS_STATUS_ERROR,
    };
    struct partial_write second = {
        .storage = &encrypted, .offset = race_block + 2048u,
        .value = 0xa7, .status = INFS_STATUS_ERROR,
    };
    pthread_t first_thread, second_thread;
    ok(pthread_create(&first_thread, NULL,
                      partial_write_thread, &first) == 0,
       "start first partial writer");
    ok(pthread_create(&second_thread, NULL,
                      partial_write_thread, &second) == 0,
       "start second partial writer");
    ok(pthread_join(first_thread, NULL) == 0 &&
       pthread_join(second_thread, NULL) == 0,
       "join partial writers");
    ok(first.status == INFS_STATUS_OK && second.status == INFS_STATUS_OK,
       "concurrent partial writes succeed");

    uint8_t concurrent_block[4096];
    ok(infs_storage_read(&encrypted, race_block,
                         concurrent_block, sizeof(concurrent_block)) ==
           INFS_STATUS_OK,
       "read concurrent partial-write block");
    for (size_t i = 0; i < 2048u; ++i)
        ok(concurrent_block[i] == 0x3c,
           "first concurrent partial write preserved");
    for (size_t i = 2048u; i < sizeof(concurrent_block); ++i)
        ok(concurrent_block[i] == 0xa7,
           "second concurrent partial write preserved");
#endif
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

#if !defined(_WIN32)
    pthread_cond_destroy(&memory.race_cond);
    pthread_mutex_destroy(&memory.race_lock);
    pthread_mutex_destroy(&memory.io_lock);
#endif
    free(memory.bytes);
    puts("authenticated encrypted storage: PASS");
    return 0;
}
